// the OkHttp implementation of ciphon_client, the default of the kotlin bindings.
package dev.ciko.ciphon

import java.io.IOException
import java.io.InterruptedIOException
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okio.Buffer

/**
 * sends the requests of ciphon libraries with OkHttp.
 *
 * libraries call it from the thread running their blocking call, the request runs synchronously there.
 * cookies are left to the libraries, which read and send them as headers.
 */
class CiphonOkHttp(private val client: OkHttpClient = OkHttpClient()) : CiphonClient {
  override fun send(request: CiphonRequest): CiphonResponse {
    val call = client.newBuilder()
      .followRedirects(request.followRedirects)
      .followSslRedirects(request.followRedirects)
      .apply { if (request.timeoutMs != 0u) callTimeout(request.timeoutMs.toLong(), TimeUnit.MILLISECONDS) }
      .build()
    val method = request.method.name
    // OkHttp refuses a body on GET and HEAD and requires one on POST, PUT and PATCH.
    val body = when (request.method) {
      CiphonMethod.GET, CiphonMethod.HEAD -> null
      else -> request.body.toRequestBody()
    }
    val builder = try {
      Request.Builder().url(request.url).method(method, body)
    } catch (e: IllegalArgumentException) {
      throw CiphonException.InvalidRequest(e.message ?: "invalid request")
    }
    for (header in request.headers) builder.addHeader(header.name, header.value)

    try {
      call.newCall(builder.build()).execute().use { response ->
        return CiphonResponse(
          status = response.code.toUShort(),
          headers = response.headers.map { (name, value) -> CiphonHeader(name, value) },
          body = read(response, request.maxResponseLen),
        )
      }
    } catch (e: SSLException) {
      throw CiphonException.Tls(e.message ?: "tls failure")
    } catch (e: InterruptedIOException) {
      throw CiphonException.Timeout(e.message ?: "timeout")
    } catch (e: IOException) {
      throw CiphonException.Network(e.message ?: "network failure")
    }
  }

  // reads at most `limit` bytes, refusing an announced or received body over it before reading the rest.
  private fun read(response: Response, limit: ULong): ByteArray {
    val body = response.body ?: return ByteArray(0)
    if (limit == 0uL || response.request.method == "HEAD") return body.bytes()
    val tooLarge = CiphonException.TooLarge("the response body exceeds $limit bytes")
    // OkHttp decodes gzip itself and then drops content-length, an announced length is the decoded one.
    if (body.contentLength() > 0 && body.contentLength().toULong() > limit) throw tooLarge
    val buffer = Buffer()
    val source = body.source()
    while (source.read(buffer, 8192) != -1L) {
      if (buffer.size.toULong() > limit) throw tooLarge
    }
    return buffer.readByteArray()
  }
}

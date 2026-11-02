// the OkHttp implementation of ciphon_client, the default of the kotlin bindings.
package dev.ciko.ciphon

import java.io.IOException
import java.io.InterruptedIOException
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

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
          body = response.body?.bytes() ?: ByteArray(0),
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
}

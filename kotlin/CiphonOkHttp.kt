// the OkHttp implementation of ciphon_client, the default of the kotlin bindings.
package dev.ciko.ciphon

import java.io.IOException
import java.io.InterruptedIOException
import java.net.InetAddress
import java.net.Proxy
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException
import okhttp3.Dns
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
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
    val schemes = schemes(request.protocols)
    val url = request.url.toHttpUrlOrNull()
    if (url != null && url.scheme !in schemes) {
      throw CiphonException.InvalidRequest("the scheme of ${request.url} is not allowed")
    }
    val call = client.newBuilder()
      .followRedirects(request.followRedirects)
      .followSslRedirects(request.followRedirects)
      .apply { if (request.timeoutMs != 0u) callTimeout(request.timeoutMs.toLong(), TimeUnit.MILLISECONDS) }
      .apply {
        if (request.connectTimeoutMs != 0u) connectTimeout(request.connectTimeoutMs.toLong(), TimeUnit.MILLISECONDS)
      }
      .apply { if (request.noProxy) proxy(Proxy.NO_PROXY) }
      .apply { if (request.pins.isNotEmpty()) dns(pinned(request.pins, url?.port)) }
      // every hop of a redirect goes through the network interceptors.
      .addNetworkInterceptor { chain ->
        val scheme = chain.request().url.scheme
        if (scheme !in schemes) throw RefusedScheme(chain.request().url.toString())
        chain.proceed(chain.request())
      }
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
        // OkHttp hands back a redirect it can't follow, such as one to another scheme.
        val location = response.header("Location")
        if (request.followRedirects && response.isRedirect && location != null) {
          val next = response.request.url.resolve(location)
          if (next == null || next.scheme !in schemes) throw RefusedScheme(location)
        }
        return CiphonResponse(
          status = response.code.toUShort(),
          headers = response.headers.map { (name, value) -> CiphonHeader(name, value) },
          body = read(response, request.maxResponseLen),
          url = response.request.url.toString(),
        )
      }
    } catch (e: RefusedScheme) {
      throw CiphonException.InvalidRequest("the scheme of ${e.message} is not allowed")
    } catch (e: SSLException) {
      throw CiphonException.Tls(e.message ?: "tls failure")
    } catch (e: InterruptedIOException) {
      throw CiphonException.Timeout(e.message ?: "timeout")
    } catch (e: IOException) {
      throw CiphonException.Network(e.message ?: "network failure")
    }
  }

  private class RefusedScheme(url: String) : IOException(url)

  @OptIn(CiffiInternal::class)
  private fun schemes(protocols: UByte): Set<String> {
    val allowed = buildSet {
      if (protocols.toInt() and CiphonProtocol.HTTP.ciffiRaw != 0) add("http")
      if (protocols.toInt() and CiphonProtocol.HTTPS.ciffiRaw != 0) add("https")
    }
    return allowed.ifEmpty { setOf("http", "https") }
  }

  // resolves the pinned hosts to their addresses only, OkHttp looks hosts up without their port.
  private fun pinned(pins: List<CiphonPin>, port: Int?): Dns {
    val addresses = pins
      .filter { it.port.toInt() == 0 || it.port.toInt() == port }
      .groupBy({ it.host.lowercase() }, { InetAddress.getByName(it.address) })
    return object : Dns {
      override fun lookup(hostname: String) = addresses[hostname.lowercase()] ?: Dns.SYSTEM.lookup(hostname)
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

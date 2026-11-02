// the net library from kotlin, sending through the default OkHttp client.
import dev.ciko.ciphon.net.*
import java.util.concurrent.CountDownLatch
import kotlin.coroutines.Continuation
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.startCoroutine
import kotlin.system.exitProcess

fun expect(condition: Boolean, what: String) {
  if (!condition) {
    println("FAIL: $what")
    exitProcess(1)
  }
}

fun <T> blocking(block: suspend () -> T): T {
  val done = CountDownLatch(1)
  var result: Result<T>? = null
  block.startCoroutine(Continuation(EmptyCoroutineContext) { result = it; done.countDown() })
  done.await()
  return result!!.getOrThrow()
}

fun main() {
  val base = System.getenv("CIPHON_TEST_URL")
  NetClient(base).use { client ->
    val hello = blocking { client.get("/hello", true) }
    expect(hello.status == 200.toUShort() && hello.body == "hello", "status and body")
    expect(hello.cookies == 2u && hello.cookie?.startsWith("session=abc") == true, "set-cookie headers stay separate")

    val redirect = blocking { client.get("/redirect", false) }
    expect(redirect.status == 302.toUShort() && redirect.location == "/hello", "redirects can be read")
    expect(blocking { client.get("/redirect", true) }.body == "hello", "redirects can be followed")
    expect(blocking { client.post("/echo", "shout") }.body == "SHOUT", "post bodies")

    expect(blocking { client.getCapped("/bytes/1000", 1000uL) }.body.length == 1000, "a body at the cap")
    for (path in listOf("/bytes/100000", "/huge", "/stream/100000")) {
      try {
        blocking { client.getCapped(path, 1000uL) }
        expect(false, "$path is over the cap")
      } catch (e: NetException.TooLarge) {
        expect(e.message!!.contains("1000"), "cap on $path: $e")
      }
    }

    try {
      blocking { client.get("/missing", true) }
      expect(false, "error statuses throw")
    } catch (e: NetException.Status) {
      expect(e.message!!.contains("404"), "error status $e")
    }
  }
  try {
    blocking { NetClient("http://127.0.0.1:1").get("/", true) }
    expect(false, "transport errors throw")
  } catch (e: NetException.Transport) {
  }

  println("kotlin: all checks passed")
}

// the net library from swift, sending through the default URLSession client.
import Foundation
import Net

func expect(_ condition: Bool, _ what: @autoclosure () -> String, line: Int = #line) {
  if !condition {
    print("FAIL line \(line): \(what())")
    exit(1)
  }
}

let base = ProcessInfo.processInfo.environment["CIPHON_TEST_URL"]!
let client = NetClient(baseUrl: base)

let hello = try await client.get(path: "/hello", follow: true)
expect(hello.status == 200 && hello.body == "hello", "status and body")
expect(hello.cookies == 2 && hello.cookie?.hasPrefix("session=abc") == true, "set-cookie headers stay separate")

let redirect = try await client.get(path: "/redirect", follow: false)
expect(redirect.status == 302 && redirect.location == "/hello", "redirects can be read")
expect(try await client.get(path: "/redirect", follow: true).body == "hello", "redirects can be followed")
expect(try await client.post(path: "/echo", body: "shout").body == "SHOUT", "post bodies")

do {
  _ = try await client.get(path: "/missing", follow: true)
  expect(false, "error statuses throw")
} catch {
  expect(error.code == 2 && error.message.contains("404"), "error status \(error)")
}
do {
  _ = try await NetClient(baseUrl: "http://127.0.0.1:1").get(path: "/", follow: true)
  expect(false, "transport errors throw")
} catch {
  expect(error == .transport(message: error.message), "transport error \(error)")
}


print("swift: all checks passed")

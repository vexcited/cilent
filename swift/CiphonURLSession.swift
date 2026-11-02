// the URLSession implementation of ciphon_client, the default of the swift bindings.
import Foundation

#if canImport(FoundationNetworking)
  import FoundationNetworking
#endif

/// sends the requests of ciphon libraries with URLSession.
///
/// libraries call it from the thread running their blocking call, it waits for URLSession there.
/// cookies are left to the libraries, which read and send them as headers.
public final class CiphonURLSession: CiphonClient {
  private let session: URLSession

  public init(configuration: URLSessionConfiguration = .ephemeral) {
    configuration.httpShouldSetCookies = false
    configuration.httpCookieAcceptPolicy = .never
    session = URLSession(configuration: configuration)
  }

  public func send(request: CiphonRequest) throws(CiphonError) -> CiphonResponse {
    guard let url = URL(string: request.url) else {
      throw .invalidRequest(message: "invalid url \(request.url)")
    }
    var urlRequest = URLRequest(url: url)
    urlRequest.httpMethod = Self.methods[request.method]
    urlRequest.httpShouldHandleCookies = false
    for header in request.headers {
      urlRequest.addValue(header.value, forHTTPHeaderField: header.name)
    }
    if !request.body.isEmpty {
      urlRequest.httpBody = Data(request.body)
    }
    if request.timeoutMs > 0 {
      urlRequest.timeoutInterval = Double(request.timeoutMs) / 1000
    }

    let outcome = Outcome()
    let done = DispatchSemaphore(value: 0)
    let task = session.dataTask(with: urlRequest) { data, response, error in
      outcome.set(data: data, response: response, error: error)
      done.signal()
    }
    task.delegate = Redirects(follow: request.followRedirects)
    task.resume()
    done.wait()
    return try outcome.response()
  }

  private static let methods: [CiphonMethod: String] = [
    .get: "GET", .head: "HEAD", .post: "POST", .put: "PUT", .patch: "PATCH", .delete: "DELETE",
  ]
}

// stops at a redirect when the library reads `location` itself.
private final class Redirects: NSObject, URLSessionTaskDelegate {
  let follow: Bool

  init(follow: Bool) {
    self.follow = follow
  }

  func urlSession(
    _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void
  ) {
    completionHandler(follow ? request : nil)
  }
}

private final class Outcome: @unchecked Sendable {
  private var data: Data?
  private var urlResponse: URLResponse?
  private var error: Error?

  func set(data: Data?, response: URLResponse?, error: Error?) {
    self.data = data
    self.urlResponse = response
    self.error = error
  }

  func response() throws(CiphonError) -> CiphonResponse {
    if let error = error as? URLError {
      throw Self.lift(error)
    }
    if let error {
      throw .network(message: error.localizedDescription)
    }
    guard let http = urlResponse as? HTTPURLResponse else {
      throw .network(message: "the response is not http")
    }
    return CiphonResponse(status: UInt16(http.statusCode), headers: Self.headers(of: http), body: [UInt8](data ?? Data()))
  }

  // URLSession joins repeated headers with commas, cookies are split back into one header each.
  private static func headers(of response: HTTPURLResponse) -> [CiphonHeader] {
    var headers: [CiphonHeader] = []
    var fields: [String: String] = [:]
    for (name, value) in response.allHeaderFields {
      guard let name = name as? String, let value = value as? String else { continue }
      if name.caseInsensitiveCompare("set-cookie") == .orderedSame {
        fields[name] = value
      } else {
        headers.append(CiphonHeader(name: name, value: value))
      }
    }
    if let url = response.url {
      for cookie in HTTPCookie.cookies(withResponseHeaderFields: fields, for: url) {
        var value = "\(cookie.name)=\(cookie.value); Path=\(cookie.path)"
        if !cookie.domain.isEmpty { value += "; Domain=\(cookie.domain)" }
        if cookie.isSecure { value += "; Secure" }
        if cookie.isHTTPOnly { value += "; HttpOnly" }
        headers.append(CiphonHeader(name: "Set-Cookie", value: value))
      }
    }
    return headers
  }

  private static func lift(_ error: URLError) -> CiphonError {
    let message = error.localizedDescription
    switch error.code {
    case .timedOut:
      return .timeout(message: message)
    case .cancelled:
      return .cancelled(message: message)
    case .badURL, .unsupportedURL:
      return .invalidRequest(message: message)
    case .secureConnectionFailed, .serverCertificateUntrusted, .serverCertificateHasBadDate,
      .serverCertificateNotYetValid, .serverCertificateHasUnknownRoot, .clientCertificateRejected,
      .clientCertificateRequired:
      return .tls(message: message)
    default:
      return .network(message: message)
    }
  }
}

// The Logger swift-foundation's Darwin build reaches through `import os.log`,
// which on Apple's SDK is the closed-source Swift `os` overlay.
// FIXME (ACCURACY): messages are dropped rather than written to the unified log.
internal struct Logger: Sendable {
    init(subsystem: String, category: String) {}

    struct Message: ExpressibleByStringInterpolation {
        struct StringInterpolation: StringInterpolationProtocol {
            enum Privacy { case `public`, `private` }
            init(literalCapacity: Int, interpolationCount: Int) {}
            mutating func appendLiteral(_ literal: String) {}
            mutating func appendInterpolation<T>(_ value: @autoclosure () -> T, privacy: Privacy = .private) {}
        }
        init(stringLiteral value: String) {}
        init(stringInterpolation: StringInterpolation) {}
    }

    func debug(_ message: Message) {}
    func info(_ message: Message) {}
    func error(_ message: Message) {}
    func fault(_ message: Message) {}
}

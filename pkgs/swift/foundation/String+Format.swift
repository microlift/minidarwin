// String's printf-style formatting, which Apple's Foundation implements on
// NSString and swift-foundation leaves to it: formatted by CoreFoundation, with
// a String argument passed to `%@` as its bridged NSString.

@_spi(Foundation) import Swift
internal import _FoundationDarwinShims


@available(macOS 10.10, iOS 8.0, watchOS 2.0, tvOS 9.0, *)
extension String: CVarArg {
    /// The bridged NSString, autoreleased as an NSObject argument is.
    public var _cVarArgEncoding: [Int] {
        let object = _bridgeToObjectiveCImpl()
        _autorelease(object)
        return [Int(bitPattern: ObjectIdentifier(object))]
    }
}

@available(macOS 10.10, iOS 8.0, watchOS 2.0, tvOS 9.0, *)
extension String {
    public init(format: __shared String, _ arguments: CVarArg...) {
        self.init(format: format, locale: nil, arguments: arguments)
    }

    public init(format: __shared String, arguments: __shared [CVarArg]) {
        self.init(format: format, locale: nil, arguments: arguments)
    }

    public init(format: __shared String, locale: __shared Locale?, _ args: CVarArg...) {
        self.init(format: format, locale: locale, arguments: args)
    }

    // FIXME (ACCURACY): `locale` is not applied. Apple's hands an NSLocale to
    // CoreFoundation as the format options; this Locale does not bridge to one,
    // so every format is the unlocalized one a nil locale gives.
    public init(format: __shared String, locale: __shared Locale?, arguments: __shared [CVarArg]) {
        let format = format._bridgeToObjectiveCImpl()
        let result = withVaList(arguments) { list in
            withExtendedLifetime(format) {
                CFStringCreateWithFormatAndArguments(
                    nil, nil, Unmanaged.passUnretained(format).toOpaque(), list)
            }
        }
        guard let result else {
            fatalError("String(format:): CoreFoundation could not format the string")
        }
        self.init(_cocoaString: Unmanaged<AnyObject>.fromOpaque(result).takeRetainedValue())
    }
}

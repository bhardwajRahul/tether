//
//  CertificateManagerTests.swift
//  TetherFrameworkTests
//

import Foundation
import Testing
@testable import TetherFramework

struct CertificateManagerTests {
    // The DER UTCTime field must be 13 ASCII bytes in the Gregorian calendar regardless
    // of the device locale; a locale-aware formatter breaks certificate generation.
    @Test func utcTimeIsGregorianASCII() {
        let encoded = CertificateManager.utcTimeString(Date(timeIntervalSince1970: 1_788_000_000))
        #expect(encoded == "260829104000Z")
        #expect(encoded.utf8.count == 13)
    }
}

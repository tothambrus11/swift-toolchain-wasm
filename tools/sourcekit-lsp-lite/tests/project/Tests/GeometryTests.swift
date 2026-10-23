import XCTest
@testable import App

final class GeometryTests: XCTestCase {
    func testNorm() {
        XCTAssertEqual(Point(x: 3, y: 4).norm(), 5)
    }

    func testDistanceIsSymmetric() {
        let a = Point(x: 1, y: 2)
        let b = Point(x: -2, y: 6)
        XCTAssertEqual(distance(a, b), distance(b, a), accuracy: 1e-12)
    }
}

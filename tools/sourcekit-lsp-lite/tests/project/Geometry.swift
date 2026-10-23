/// A point in the plane.
struct Point: CustomStringConvertible {
    var x: Double
    var y: Double

    /// Distance from the origin.
    func norm() -> Double {
        (x * x + y * y).squareRoot()
    }

    var description: String { "(\(x), \(y))" }
}

/// Euclidean distance between two points.
func distance(_ a: Point, _ b: Point) -> Double {
    Point(x: a.x - b.x, y: a.y - b.y).norm()
}

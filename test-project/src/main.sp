import "math"
import "types"

fn main() -> int {
    let a: int = 10
    let b: int = 25
    let sum: int = math.add(a, b)
    let diff: int = math.subtract(b, a)
    let prod: int = math.multiply(a, b)

    println("Sum: {}", sum)
    println("Difference: {}", diff)
    println("Product: {}", prod)

    return 0
}

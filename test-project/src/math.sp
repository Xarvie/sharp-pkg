import "types"

pub fn add(a: int, b: int) -> int {
    return a + b
}

pub fn subtract(a: int, b: int) -> int {
    return a - b
}

pub fn multiply(a: int, b: int) -> int {
    return a * b
}

pub fn divide(a: int, b: int) -> Result {
    if b == 0 {
        return Result { ok: false, value: 0 }
    }
    return Result { ok: true, value: a / b }
}

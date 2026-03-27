; REQUIRES: geq-llvm-15.0
; RUN: %S/ConcreteTest.py --klee='%klee' --lli=%lli %s

; Most of the test below use the *address* of gInt as part of their computation,
; and then perform some operation (like x | ~x) which makes the result
; deterministic. They do, however, assume that the sign bit of the address as a
; 64-bit value will never be set.
@gInt = global i32 10
@gIntWithConstant = global i32 0

define void @"test_int_to_ptr"() {
  ; Use inttoptr/ptrtoint roundtrips via i64 to avoid lli APInt bug
  ; on 64-bit systems where ptrtoint-to-i32 of a high address overflows.
  %p1 = inttoptr i64 100 to i8*
  %t1.tmp = ptrtoint i8* %p1 to i64
  %t1 = trunc i64 %t1.tmp to i8

  %p2 = inttoptr i64 100 to i32*
  %t2.tmp = ptrtoint i32* %p2 to i64
  %t2 = trunc i64 %t2.tmp to i32

  %p3 = inttoptr i64 100 to i32*
  %t3.tmp = ptrtoint i32* %p3 to i64
  %t3 = trunc i64 %t3.tmp to i32

  %p4 = inttoptr i64 100 to i8*
  %t4 = ptrtoint i8* %p4 to i64

  call void @print_i8(i8 %t1)
  call void @print_i32(i32 %t2)
  call void @print_i32(i32 %t3)
  call void @print_i64(i64 %t4)

  ret void
}

define void @"test_constant_ops"() {
  %addr64 = ptrtoint i32* @gInt to i64

  ; trunc(addr + -10) to i8, then add 10
  %a1 = add i64 %addr64, -10
  %a2 = trunc i64 %a1 to i8
  %t1 = add i8 %a2, 10

  ; sext(trunc(addr, 32), 64) - addr, masked to 32 bits
  %addr32 = trunc i64 %addr64 to i32
  %sext_addr = sext i32 %addr32 to i64
  %s1 = sub i64 %sext_addr, %addr64
  %t2 = and i64 %s1, 4294967295

  ; zext(trunc(addr, 32), 64) - addr, masked to 32 bits
  %zext_addr = zext i32 %addr32 to i64
  %s2 = sub i64 %zext_addr, %addr64
  %t3 = and i64 %s2, 4294967295

  ; icmp trunc(addr) == t1
  %trunc_addr = trunc i64 %addr64 to i8
  %t4 = icmp eq i8 %trunc_addr, %t1
  %t5 = zext i1 %t4 to i8

  call void @print_i8(i8 %t5)
  call void @print_i64(i64 %t2)
  call void @print_i64(i64 %t3)

  ret void
}

define void @"test_logical_ops"() {
  %addr64 = ptrtoint i32* @gInt to i64
  %addr32 = trunc i64 %addr64 to i32

  ; and(addr, ~addr) == 0, so add -10 => -10
  %not_addr = xor i32 %addr32, -1
  %and1 = and i32 %addr32, %not_addr
  %t1 = add i32 -10, %and1

  ; or(addr, ~addr) == -1, so add -10 => -11
  %or1 = or i32 %addr32, %not_addr
  %t2 = add i32 -10, %or1

  ; xor(xor(addr, 1024), addr) == 1024, so add -10 => 1014
  %xor1 = xor i32 %addr32, 1024
  %xor2 = xor i32 %xor1, %addr32
  %t3 = add i32 -10, %xor2

  call void @print_i32(i32 %t1)
  call void @print_i32(i32 %t2)
  call void @print_i32(i32 %t3)

  ; or the address with 1 to ensure the addresses will differ in 'ne' below
  %or_addr = or i64 %addr64, 1

  %lshr1 = lshr i64 %or_addr, 8
  %t4 = shl i64 %lshr1, 8

  %ashr1 = ashr i64 %or_addr, 8
  %t5 = shl i64 %ashr1, 8

  %shl1 = shl i64 %or_addr, 8
  %t6 = lshr i64 %shl1, 8

  %t7 = icmp eq i64 %t4, %t5
  %t8 = icmp ne i64 %t4, %t6

  %t9 = zext i1 %t7 to i8
  %t10 = zext i1 %t8 to i8

  call void @print_i8(i8 %t9)
  call void @print_i8(i8 %t10)

  ret void
}

%test.struct.type = type { i32, i32 }
@test_struct = global %test.struct.type { i32 0, i32 10 }

define void @"test_misc"() {
  ; probability that @gInt == 100 is very very low
  %cmp_ptr = inttoptr i64 100 to i32*
  %cmp = icmp eq i32* @gInt, %cmp_ptr
  %sel = select i1 %cmp, i32 10, i32 0
  %t1 = add i32 %sel, 0
  call void @print_i32(i32 %t1)

  %gep = getelementptr %test.struct.type, %test.struct.type* @test_struct, i32 0, i32 1
  %t2 = load i32, i32* %gep
  call void @print_i32(i32 %t2)

  ret void
}

define void @"test_simple_arith"() {
  %addr64 = ptrtoint i32* @gInt to i64
  %addr32 = trunc i64 %addr64 to i32

  ; add(addr, 0) + 0
  %a1 = add i32 %addr32, 0
  %t1 = add i32 %a1, 0

  ; sub(0, addr) + t1 => 0
  %s1 = sub i32 0, %addr32
  %t2 = add i32 %s1, %t1

  ; mul(addr, 10) * 0 => 0
  %m1 = mul i32 %addr32, 10
  %t3 = mul i32 %m1, %t2

  call void @print_i32(i32 %t3)

  ret void
}

define void @test_cmp() {
  %addr64 = ptrtoint i32* @gInt to i64

  %c1 = icmp ult i64 %addr64, 0
  %z1 = zext i1 %c1 to i8
  %t1 = add i8 %z1, 1

  %c2 = icmp ule i64 %addr64, 0
  %z2 = zext i1 %c2 to i8
  %t2 = add i8 %z2, 1

  %c3 = icmp uge i64 %addr64, 0
  %z3 = zext i1 %c3 to i8
  %t3 = add i8 %z3, 1

  %c4 = icmp ugt i64 %addr64, 0
  %z4 = zext i1 %c4 to i8
  %t4 = add i8 %z4, 1

  %c5 = icmp slt i64 %addr64, 0
  %z5 = zext i1 %c5 to i8
  %t5 = add i8 %z5, 1

  %c6 = icmp sle i64 %addr64, 0
  %z6 = zext i1 %c6 to i8
  %t6 = add i8 %z6, 1

  %c7 = icmp sge i64 %addr64, 0
  %z7 = zext i1 %c7 to i8
  %t7 = add i8 %z7, 1

  %c8 = icmp sgt i64 %addr64, 0
  %z8 = zext i1 %c8 to i8
  %t8 = add i8 %z8, 1

  %c9 = icmp eq i64 %addr64, 10
  %z9 = zext i1 %c9 to i8
  %t9 = add i8 %z9, 1

  %c10 = icmp ne i64 %addr64, 10
  %z10 = zext i1 %c10 to i8
  %t10 = add i8 %z10, 1

  call void @print_i1(i8 %t1)
  call void @print_i1(i8 %t2)
  call void @print_i1(i8 %t3)
  call void @print_i1(i8 %t4)
  call void @print_i1(i8 %t5)
  call void @print_i1(i8 %t6)
  call void @print_i1(i8 %t7)
  call void @print_i1(i8 %t8)
  call void @print_i1(i8 %t9)
  call void @print_i1(i8 %t10)

  ret void
}

define i32 @main() {
    ; Initialize gIntWithConstant to 0
    store i32 0, i32* @gIntWithConstant

    call void @test_simple_arith()

    call void @test_cmp()

    call void @test_int_to_ptr()

    call void @test_constant_ops()

    call void @test_logical_ops()

    call void @test_misc()

    ret i32 0
}

; defined in print_int.c
declare void @print_i1(i8)
declare void @print_i8(i8)
declare void @print_i16(i16)
declare void @print_i32(i32)
declare void @print_i64(i64)

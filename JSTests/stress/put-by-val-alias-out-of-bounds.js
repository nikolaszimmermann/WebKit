let array = new Uint32Array(65535);
function test() {
    for (let i = 0; i < testLoopCount; i += 6)
        array[i] &= 2;
    return array;
}
noInline(test);

test();
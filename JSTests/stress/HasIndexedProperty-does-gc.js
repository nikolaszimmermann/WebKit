// This test should not crash.
function foo(a) {
    return 0 in a;
}
for (let i = 0; i < testLoopCount; i++) {
    const str = new String('asdf');
    str[42] = 'x'; // Give it ArrayStorage
    foo(str);
}

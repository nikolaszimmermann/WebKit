function Cons() {
}
Cons.prototype.__defineGetter__("f", function() {
    counter++;
    return 84;
});

function foo(o) {
    return o.f;
}

noInline(foo);

var counter = 0;

function test(o, expected, expectedCount) {
    var result = foo(o);
    if (result != expected)
        throw new Error("Bad result: " + result);
    if (counter != expectedCount)
        throw new Error("Bad counter value: " + counter);
}

for (var i = 0; i < testLoopCount; ++i) {
    test(new Cons(), 84, counter + 1);
    
    var o = new Cons();
    o.g = 54;
    test(o, 84, counter + 1);
}

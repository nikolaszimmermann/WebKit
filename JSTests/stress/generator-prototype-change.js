function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

function* gen()
{
}
var originalPrototype = gen.prototype;

for (var i = 0; i < testLoopCount; ++i) {
    var g = gen();
    shouldBe(g.__proto__, gen.prototype);
}
gen.prototype = {};
var g = gen();
shouldBe(g.__proto__, gen.prototype);
shouldBe(g.__proto__ !== originalPrototype, true);

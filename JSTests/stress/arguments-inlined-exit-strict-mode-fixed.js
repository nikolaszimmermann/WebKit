"use strict";

function foo(x) {
    var tmp = x.f + 1;
    return tmp + arguments[0].f;
}

function bar(x) {
    return foo(x);
}

noInline(bar);

for (var i = 0; i < testLoopCount; ++i) {
    var result = bar({f:i});
    if (result != i + i + 1)
        throw "Error: bad result: " + result;
}

var result = bar({f:4.5});
if (result != 4.5 + 4.5 + 1)
    throw "Error: bad result at end: " + result;

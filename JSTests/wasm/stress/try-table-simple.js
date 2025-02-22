
import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

/*
(module
  (type $0 (func))
  (type $1 (func (param i32) (result i32)))
  (tag $0 (type 0))
  (export "simple_throw_catch" (func 0))
  (func $0
    (type 1)
    (block
      (try_table
        (result i32)
        (catch 0 0)
        (local.get 0)
        (i32.eqz)
        (if (then (throw 0)) (else))
        (i32.const 42)
      )
      (return)
    )
    (i32.const 23)
  )
)
*/

var wasm_module = new WebAssembly.Module(new Uint8Array([
0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x89,0x80,0x80,0x80,0x00,0x02,0x60,0x00,0x00,0x60,0x01,0x7f,0x01,0x7f,0x03,0x82,0x80,0x80,0x80,0x00,0x01,0x01,0x0d,0x83,0x80,0x80,0x80,0x00,0x01,0x00,0x00,0x07,0x96,0x80,0x80,0x80,0x00,0x01,0x12,0x73,0x69,0x6d,0x70,0x6c,0x65,0x5f,0x74,0x68,0x72,0x6f,0x77,0x5f,0x63,0x61,0x74,0x63,0x68,0x00,0x00,0x0a,0x9f,0x80,0x80,0x80,0x00,0x01,0x99,0x80,0x80,0x80,0x00,0x00,0x02,0x40,0x1f,0x7f,0x01,0x00,0x00,0x00,0x20,0x00,0x45,0x04,0x40,0x08,0x00,0x0b,0x41,0x2a,0x0b,0x0f,0x0b,0x41,0x17,0x0b
]));

var instance = new WebAssembly.Instance(wasm_module);

const { simple_throw_catch } = instance.exports

for (let i = 0; i < wasmTestLoopCount; ++i) {
    assert.eq(simple_throw_catch(0), 23)
    assert.eq(simple_throw_catch(1), 42)
}

/* 用 Node 校验 JS 推理与 Python 期望值一致。
 * 运行：node verify_js.js
 */
const fs = require("fs");
const path = require("path");

if (typeof atob === "undefined") {
  global.atob = (s) => Buffer.from(s, "base64").toString("binary");
}

const root = path.join(__dirname, "..");
const app = path.join(root, "web");

eval(fs.readFileSync(path.join(app, "wq_weights.js"), "utf8"));
eval(fs.readFileSync(path.join(app, "wq_cnn.js"), "utf8"));

const data = JSON.parse(fs.readFileSync(path.join(__dirname, "test_vectors.json"), "utf8"));
let ok = true;
for (const c of data.cases) {
  const r = WQ.predict(c.window);
  let maxd = 0;
  for (let i = 0; i < 4; i++) {
    maxd = Math.max(maxd, Math.abs(r.probs[i] - c.expected.probs[i]));
  }
  const pass = r.cls === c.expected.cls && maxd < 1e-5;
  ok = ok && pass;
  console.log(
    `${c.name.padEnd(6)} cls=${r.cls} maxdiff=${maxd.toExponential(2)} ${pass ? "PASS" : "FAIL"}`
  );
}
console.log(ok ? "RESULT: PASS" : "RESULT: FAIL");
process.exit(ok ? 0 : 1);

const buffer = require("buffer");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
class DecipherUtil {
  static decryptPwd(r, e) {
    let t = buffer.Buffer.from("");
    try {
      const i = DecipherUtil.getKey(r),
            o = new Int8Array(buffer.Buffer.from(e, "hex"));
      t = DecipherUtil.decrypt(i, o)
    } catch (e) {
      console.log(e);
    }
    return t.toString("utf-8")
  }
  static getKey(r) {
    const e = path.resolve(r, "material");
    const t = fs.readdirSync(e).filter((r => r != this.macDSStore));
    const i = this.readFd(path.resolve(e, this.dirs[0])),
          o = this.readSalt(path.resolve(e, this.dirs[1])),
          a = this.getRootKey(i, o),
          s = this.readWorkMaterial(path.resolve(e, this.dirs[2]));
    return new Int8Array(DecipherUtil.decrypt(a, s))
  }
  static getRootKey(r, e) {
    const t = r.concat(this.component), i = this.xorComponents(t),
          o = crypto.pbkdf2Sync(i.toString(), e, 1e4, 16, "sha256");
    return new Int8Array(o)
  }
  static decrypt(r, e) {
    const t = (255 & e[0]) << 24 | (255 & e[1]) << 16 | (255 & e[2]) << 8 |
              255 & e[3],
          i = e.length - 4 - t, o = e.slice(4, 4 + i),
          a = crypto.createDecipheriv("aes-128-gcm", r, o),
          s = e.slice(e.length - 16);
    a.setAuthTag(s);
    const l = a.update(e.subarray(4 + i, e.length - 16)), n = a.final();
    return buffer.Buffer.concat([ l, n ])
  }
  static xorComponents(r) {
    let e = this.xor(r[0], r[1]);
    for (let t = 2; t < r.length; t++)
      e = this.xor(e, r[t]);
    return buffer.Buffer.from(e)
  }
  static xor(r, e) {
    const t = new Int8Array(r.byteLength);
    for (let i = 0; i < r.byteLength; i++)
      t[i] = r[i] ^ e[i];
    return t
  }
  static readFd(r) {
    const e = fs.readdirSync(r).filter((r => r != this.macDSStore));
    const t = [];
    return e.forEach(
               (e => t.push(this.readDirBytes(path.resolve(r, e))))),
           t
  }
  static readSalt(r) { return this.readDirBytes(r) }
  static readWorkMaterial(r) { return this.readDirBytes(r) }
}

DecipherUtil.component = new Int8Array([
  49, 243, 9, 115, 214, 175, 91, 184, 211, 190, 177, 88, 101, 131, 192, 119
]),
DecipherUtil.dirs = [ "fd", "ac", "ce" ];
DecipherUtil.macDSStore = ".DS_Store";
DecipherUtil.readDirBytes = r => {
  const e = fs.readdirSync(r);
  const t = fs.readFileSync(path.resolve(r, e[0]));
  return new Int8Array(t);
};

console.log(DecipherUtil.decryptPwd(process.argv[2], process.argv[3]));

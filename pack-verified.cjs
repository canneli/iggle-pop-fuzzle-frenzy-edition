const fs = require('node:fs');
const path = require('node:path');

const SEED = 0x53414646;
const SHIFTS = [[7, 12, 17, 22], [5, 9, 14, 20], [4, 11, 16, 23], [6, 10, 15, 21]];
const KEYS = Array.from({ length: 64 }, (_, i) => (Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);

function safDigest(payload) {
  const padding = (119 - payload.length % 64) % 64;
  const input = Buffer.alloc(payload.length + 1 + padding + 8);
  payload.copy(input);
  input[payload.length] = 0x80;
  input.writeBigUInt64LE(BigInt(payload.length) * 8n, input.length - 8);
  let a = (SEED * 11 + 0x67452301) | 0;
  let b = (SEED * 71 - 0x10325477) | 0;
  let c = (SEED * 37 - 0x67452302) | 0;
  let d = (SEED * 97 + 0x10325476) | 0;
  for (let offset = 0; offset < input.length; offset += 64) {
    let p = a, q = b, r = c, s = d;
    for (let i = 0; i < 64; i++) {
      const round = i >>> 4;
      let mix, word;
      switch (round) {
        case 0: mix = (q & r) | (~q & s); word = i; break;
        case 1: mix = (s & q) | (~s & r); word = (5 * i + 1) % 16; break;
        case 2: mix = q ^ r ^ s; word = (3 * i + 5) % 16; break;
        case 3: mix = r ^ (q | ~s); word = (7 * i) % 16; break;
      }
      const value = (p + mix + KEYS[i] + input.readUInt32LE(offset + word * 4)) | 0;
      const shift = SHIFTS[round][i % 4];
      const next = (q + ((value << shift) | (value >>> (32 - shift)))) | 0;
      p = s;
      s = r;
      r = q;
      q = next;
    }
    a = (a + p) | 0;
    b = (b + q) | 0;
    c = (c + r) | 0;
    d = (d + s) | 0;
  }
  const result = Buffer.alloc(16);
  [a, b, c, d].forEach((value, i) => result.writeUInt32LE(value >>> 0, i * 4));
  return result;
}

function readArchive(bytes) {
  if (bytes.length < 36 || bytes.subarray(0, 8).toString('hex') !== '4646415301000000') {
    throw new Error('Unsupported SAF header');
  }
  const index = bytes.readUInt32LE(8);
  if (index < 12 || index > bytes.length - 24 || bytes.readUInt32LE(index) !== 1) {
    throw new Error('Invalid SAF directory');
  }
  const toc = Buffer.from(bytes.subarray(index));
  if (!safDigest(toc.subarray(20)).equals(toc.subarray(4, 20))) {
    throw new Error('SAF directory integrity check failed');
  }
  const count = toc.readUInt32LE(20);
  const entries = new Map();
  let cursor = 24;
  for (let i = 0; i < count; i++) {
    if (cursor + 26 > toc.length) throw new Error('Truncated SAF directory entry');
    const start = toc.readUInt32LE(cursor);
    const end = start + toc.readUInt32LE(cursor + 4);
    const nameEnd = cursor + 26 + toc.readUInt16LE(cursor + 24);
    if (nameEnd <= cursor + 27 || nameEnd > toc.length || toc[nameEnd - 1] !== 0 ||
        start < 12 || end > index) {
      throw new Error('Invalid SAF directory entry');
    }
    const name = toc.toString('ascii', cursor + 26, nameEnd - 1).toLowerCase();
    if (entries.has(name) || !name.split('/').every(part => /^[a-z0-9_. -]+$/.test(part) && part !== '.' && part !== '..')) {
      throw new Error(`Invalid SAF resource name: ${name}`);
    }
    const data = bytes.subarray(start, end);
    if (!safDigest(data).equals(toc.subarray(cursor + 8, cursor + 24))) {
      throw new Error(`SAF resource integrity check failed: ${name}`);
    }
    entries.set(name, cursor);
    cursor = nameEnd;
  }
  if (cursor !== toc.length) throw new Error('Unexpected data after SAF directory');
  return { index, toc, entries };
}

function repack(inputFile, replacementsFolder, outputFile) {
  const input = fs.readFileSync(inputFile);
  const { index, toc, entries } = readArchive(input);
  const payload = Buffer.from(input.subarray(0, index));
  const replacements = [];
  let offset = index;
  for (const [name, cursor] of entries) {
    const file = path.join(replacementsFolder, ...name.split('/'));
    if (!fs.existsSync(file)) continue;
    const data = fs.readFileSync(file);
    if (offset + data.length > 0xffffffff) throw new Error('SAF size exceeds the format limit');
    toc.writeUInt32LE(offset, cursor);
    toc.writeUInt32LE(data.length, cursor + 4);
    safDigest(data).copy(toc, cursor + 8);
    replacements.push(data);
    offset += data.length;
  }
  const added = [];
  for (const filename of fs.readdirSync(replacementsFolder, {recursive:true}).sort()) {
    const name = filename.split(path.sep).join('/');
    if (!(name.startsWith('res/monsters/hd/') || name.startsWith('res/menuassets/hd/') || name.startsWith('res/sounds/hd/')) || entries.has(name)) continue;
    const file = path.join(replacementsFolder, filename);
    if (!fs.statSync(file).isFile()) continue;
    const data = fs.readFileSync(file);
    if (!/^[a-z0-9_.\/-]+$/.test(name) || offset+data.length > 0xffffffff) {
      throw new Error('Invalid added sprite resource');
    }
    const entry = Buffer.alloc(26+name.length+1);
    entry.writeUInt32LE(offset, 0);
    entry.writeUInt32LE(data.length, 4);
    safDigest(data).copy(entry, 8);
    entry.writeUInt16LE(name.length+1, 24);
    entry.write(name, 26, 'ascii');
    added.push(entry);
    replacements.push(data);
    offset += data.length;
  }
  if (!replacements.length) throw new Error('No replacement matched an SAF resource');
  payload.writeUInt32LE(offset, 8);
  const directory = Buffer.concat([toc, ...added]);
  directory.writeUInt32LE(entries.size+added.length, 20);
  safDigest(directory.subarray(20)).copy(directory, 4);
  const output = Buffer.concat([payload, ...replacements, directory]);
  readArchive(output);
  fs.writeFileSync(outputFile, output, { flag: 'w' });
  return replacements.length;
}


const [original, resources, outputDirectory] = process.argv.slice(2);
if (!original || !resources || !outputDirectory) {
  console.error('Usage: node pack-verified.cjs <original.saf> <language-resource-directory> <output-directory>');
  process.exit(2);
}
fs.mkdirSync(outputDirectory, { recursive: true });
const source=fs.readFileSync(original), base=source.readUInt32LE(8);
for(const lang of ['en','pt-BR','es']){
 const output=path.join(outputDirectory,`IgglePop-lang-${lang}.saf`);
 console.log(lang,repack(original,path.join(resources,lang),output));
 const packed=fs.readFileSync(output);
 fs.writeFileSync(path.join(outputDirectory,lang),Buffer.concat([packed.subarray(8,12),packed.subarray(base)]));
}

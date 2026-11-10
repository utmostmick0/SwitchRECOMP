"""nupread.py: reads NUP containers (.nup), a format for already-decrypted Switch / Switch 2 dumps, written from its published
specification (github.com/a55uka/nup, SPEC.md, format 1.0; MIT). Our own code: only reading and extracting, no keys anywhere (NUP holds
plain files). The builder (nxs2.py) unpacks the program it needs into the build folder and then reads it like a dump folder.

Checked as the spec asks before anything is trusted: the superblock, the table of contents' SHA-256, every record's bounds, the codecs and
kinds this reader knows, and file names (labels, never paths: no separators, no "." or "..", nothing a file system would read specially).
Every extracted file's SHA-256 is checked against its record. zstd needs Python's zstandard module (python -m pip install zstandard)."""
import hashlib, os, pathlib, struct

MAGIC = b'NUP\x01'
TOC_MAGIC = b'TOC\x01'
KINDS = {0: 'base', 1: 'update', 2: 'addon', 3: 'addon_update', 4: 'system'}
NONE = 0xFFFFFFFF
CHUNK = 1 << 20


class NupError(Exception):
    pass


class Nup:
    """An open .nup: packages (dicts: title_id, root, exefs, romfs, version, kind, standalone), and the directory tree below each."""
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.f = open(self.path, 'rb')
        try: self._load()
        except Exception: self.f.close(); raise

    def close(self): self.f.close()
    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def _load(self):
        size = os.fstat(self.f.fileno()).st_size
        sb = self.f.read(0x80)
        if len(sb) < 0x80 or sb[:4] != MAGIC: raise NupError(f'{self.path.name} is not a NUP container (format 1)')
        hsize, hver, toc_off, toc_size = struct.unpack_from('<HHQQ', sb, 4)
        toc_hash = sb[0x18:0x38]
        blob_off, file_size, npkg, flags = struct.unpack_from('<QQII', sb, 0x38)
        self.uuid = sb[0x50:0x60].hex()
        if hsize < 0x80 or toc_off != hsize or blob_off % 16 or blob_off < toc_off + toc_size or file_size != size:
            raise NupError(f'{self.path.name}: the header does not match the file (cut short or damaged?)')
        if flags: raise NupError(f'{self.path.name}: header flags {flags:#x} are from a newer NUP version than this reader knows')
        if not 1 <= npkg <= 4096: raise NupError(f'{self.path.name}: {npkg} packages')
        self.f.seek(toc_off); toc = self.f.read(toc_size)
        if len(toc) != toc_size or hashlib.sha256(toc).digest() != toc_hash: raise NupError(f'{self.path.name}: the table of contents is damaged (hash mismatch)')
        if toc[:4] != TOC_MAGIC: raise NupError(f'{self.path.name}: no table of contents')
        tnpkg, ndirs, nfiles, pool_off, pool_size = struct.unpack_from('<IQQQQ', toc, 4)
        if tnpkg != npkg: raise NupError(f'{self.path.name}: the package counts disagree')
        pkg_at, dir_at = 0x30, 0x30 + 0x40 * npkg
        file_at = dir_at + 0x18 * ndirs
        child_at = file_at + 0x50 * nfiles
        if child_at > len(toc) or pool_off + pool_size > len(toc):
            raise NupError(f'{self.path.name}: the table of contents is inconsistent')
        self.pool = toc[pool_off:pool_off + pool_size]
        self.ndirs = ndirs
        self.dirs = [struct.unpack_from('<IIII', toc, dir_at + 0x18 * i) for i in range(ndirs)]   # name_off, name_len, first_child, child_count
        self.files = []
        for i in range(nfiles):
            name_off, name_len, fsize, ext_off, ext_size = struct.unpack_from('<IIQQQ', toc, file_at + 0x50 * i)
            sha = toc[file_at + 0x50 * i + 0x20:file_at + 0x50 * i + 0x40]; codec = toc[file_at + 0x50 * i + 0x40]
            if codec not in (0, 1): raise NupError(f'{self.path.name}: file codec {codec} is not one this reader knows (0 raw, 1 zstd)')
            if ext_size and (ext_off < blob_off or ext_off + ext_size > size or ext_off % 16): raise NupError(f'{self.path.name}: a file extent lies outside the data')
            if codec == 0 and ext_size != fsize: raise NupError(f'{self.path.name}: a stored file has the wrong size')
            self.files.append((name_off, name_len, fsize, ext_off, ext_size, sha, codec))
        nslots = sum(d[3] for d in self.dirs)
        if child_at + 4 * nslots > len(toc): raise NupError(f'{self.path.name}: the children array runs past the table of contents')
        self.children = struct.unpack_from(f'<{nslots}I', toc, child_at) if nslots else ()
        for d in self.dirs:
            if d[2] + d[3] > nslots: raise NupError(f'{self.path.name}: a directory lists children past the array')
        self.packages = []
        for i in range(npkg):
            title, root, exefs, romfs, version, kind, pflags = struct.unpack_from('<QIIIIBB', toc, pkg_at + 0x40 * i)
            if kind >= 5: raise NupError(f'{self.path.name}: package kind {kind} is not one this reader knows')
            for ix in (root, exefs, romfs):
                if ix != NONE and ix >= ndirs: raise NupError(f'{self.path.name}: a package points past the directories')
            self.packages.append({'title_id': title, 'root': root, 'exefs': None if exefs == NONE else exefs, 'romfs': None if romfs == NONE else romfs,
                                  'version': version, 'kind': KINDS.get(kind, str(kind)), 'standalone': bool(pflags & 1)})

    def _name(self, off, ln):
        if off + ln > len(self.pool): raise NupError(f'{self.path.name}: a name lies outside the string pool')
        name = self.pool[off:off + ln].decode('utf-8', 'strict')
        bad = '<>:"|?*' if os.name == 'nt' else ''
        if not name or name in ('.', '..') or any(c in name for c in '/\\\0' + bad) or any(ord(c) < 0x20 for c in name):
            raise NupError(f'{self.path.name}: the file name {name!r} cannot be used as a file name here')
        return name

    def walk(self, dir_index, prefix='', depth=0):
        """(relative path, file index) of every file below a directory, in the container's order."""
        if depth > 64: raise NupError(f'{self.path.name}: directories nested too deep (a loop?)')
        _, _, first, count = self.dirs[dir_index]
        for node in self.children[first:first + count]:
            if node < self.ndirs:
                d = self.dirs[node]
                yield from self.walk(node, f'{prefix}{self._name(d[0], d[1])}/', depth + 1)
            elif node - self.ndirs < len(self.files):
                fi = node - self.ndirs; f = self.files[fi]
                yield f'{prefix}{self._name(f[0], f[1])}', fi
            else: raise NupError(f'{self.path.name}: a directory lists a node that does not exist')

    def size_of(self, dir_index):
        return sum(self.files[fi][2] for _, fi in self.walk(dir_index))

    def extract_file(self, fi, dest):
        """One file to dest (written to a temporary name, renamed when its size and SHA-256 match its record)."""
        _, _, fsize, ext_off, ext_size, sha, codec = self.files[fi]
        dest = pathlib.Path(dest); dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + '.part')
        h = hashlib.sha256(); n = 0
        with open(tmp, 'wb') as out:
            if ext_size:
                self.f.seek(ext_off)
                if codec == 0:
                    left = ext_size
                    while left:
                        b = self.f.read(min(CHUNK, left))
                        if not b: raise NupError(f'{self.path.name}: the file ends early')
                        out.write(b); h.update(b); n += len(b); left -= len(b)
                else:
                    try: import zstandard
                    except ImportError: raise NupError('this .nup is compressed with zstd: install Python\'s zstandard module (python -m pip install zstandard), then try again')
                    reader = zstandard.ZstdDecompressor().stream_reader(_Limited(self.f, ext_size), read_across_frames=False)
                    while True:
                        b = reader.read(CHUNK)
                        if not b: break
                        n += len(b)
                        if n > fsize: raise NupError(f'{self.path.name}: a file unpacks larger than its record says')
                        out.write(b); h.update(b)
        if n != fsize or h.digest() != sha:
            tmp.unlink()
            raise NupError(f'{self.path.name}: {dest.name} does not match its checksum (the .nup is damaged)')
        os.replace(tmp, dest)


class _Limited:   # a file object that ends after n bytes: zstd reads one file's extent and no further
    def __init__(self, f, n): self.f, self.left = f, n
    def read(self, k=-1):
        if self.left <= 0: return b''
        b = self.f.read(self.left if k is None or k < 0 else min(k, self.left))
        self.left -= len(b)
        return b

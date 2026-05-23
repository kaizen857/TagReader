# TagReader fuzz corpus

本目录只保存 corpus 生成脚本，不提交二进制 seed，避免仓库体积膨胀。

运行：

```bash
python3 test/corpus/generate_corpus.py
```

默认输出目录：`/tmp/opencode/tagreader_fuzz_corpus`。

覆盖类别：

- `id3`：最小 MP3+ID3v2.2、ID3v2.3、ID3v2.4，以及截断/超限变体。
- `flac`：最小 FLAC block chain、截断 block、超限 block。
- `ogg`：最小 Ogg Vorbis page、截断 page、异常 continuation page。
- `mp4`：最小 MP4 atom tree、截断 atom、深嵌套 atom。
- `image`：PNG/APIC、截断图片、超限图片 payload。
- `encoding`：UTF-8、UTF-16、非法编码文本 payload。

每类至少 3 个 seed，覆盖合法、截断、超限或异常变体。

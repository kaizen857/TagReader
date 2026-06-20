# TagReader fuzz corpus

本目录只保存 corpus 生成脚本，不提交二进制 seed，避免仓库体积膨胀。

运行：

```bash
python3 test/corpus/generate_corpus.py
```

默认输出目录：`/tmp/opencode/tagreader_fuzz_corpus`。

如果要先确认测试入口，再跑：`cmake --preset default`、`cmake --build --preset default`、`ctest --preset default --output-on-failure`。

`clangd` 读取的编译数据库是 `build/default/compile_commands.json`。

实际生成类别与 seed 主题：

- `id3`：ID3v2.2/v2.3/v2.4 dispatch、未知大帧跳过、ID3v2.4 tag/frame unsync、footer/extended header、APIC unsync、公开 API 多字段组合，以及歌词行数和 LRC timestamp DoS 样本。
- `flac`：FLAC Vorbis comment block、截断/超限 block、PICTURE 合法样本、PICTURE mime/description/image 截断，以及 Vorbis 非法 key/value/lyrics entry 后续字段隔离。
- `ogg`：Ogg Vorbis comment page、截断 page、异常 continuation、payload 边界截断/超限、公开 API comment/LRC 组合，以及 Vorbis 非法 entry 后续字段隔离。
- `mp4`：MP4 atom tree、截断 atom、深嵌套 atom、multi-data item、meta payload/version 异常、extended atom、嵌套 item、公开 API metadata/lyrics 组合、零尺寸 atom 边界，以及 `covr` 无效后有效数据。
- `image`：ID3 APIC PNG 封面、截断图片、超限图片 payload，用于封面解码和 cover cache 路径。
- `encoding`：UTF-8、UTF-16、非法 UTF-8、大 Latin-1、奇数字节 UTF-16、MP4 UTF-16 超限文本。

脚本输出是确定性的；README 只描述当前脚本实际生成的分类和主题。

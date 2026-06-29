# @codejoo/ffuzzy

[English](README.md) | 中文

为 Web 提供的高性能模糊搜索 —— [ffuzzy](https://github.com/icodejoo/ffuzzy) C 引擎的 WASM 移植版。

五种搜索模式 · TypeScript · 浏览器 + Node · 完整版 ~57 KB / lite 版 ~43 KB

## 安装

```sh
npm install @codejoo/ffuzzy
```

## 快速上手

WASM 模块由库内部管理——启动时调用一次 `ffuzzyInitialize()`，之后同步使用
`FuzzyCorpus`，API 与 Dart 版完全对齐（无需传模块句柄）。

```ts
import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy';

await ffuzzyInitialize();   // 启动时调用一次（WASM 实例化是异步的）

// 纯字符串
const corpus = FuzzyCorpus.strings(['src/main.ts', 'README.md', 'package.json']);
corpus.fuzzy('src').forEach(h => console.log(h.raw, h.score));
corpus.dispose();

// 任意对象 —— 命中携带原对象
const files = new FuzzyCorpus(myFiles, { stringOf: f => f.path });
const hit = files.prefix('src/')[0];
hit.raw;  // 原始对象
files.dispose();
```

> 为什么要一次 `await`？浏览器禁止同步编译大于 4 KB 的 WASM 模块，所以引擎
> 必须异步初始化。初始化完成后，所有调用都是同步的。

## Lite 版

体积比完整版小 ~14 KB；覆盖 ASCII + CJK。不支持西里尔文/希腊文大小写折叠或变音符去除。

```ts
import { ffuzzyInitialize, FuzzyCorpus } from '@codejoo/ffuzzy/lite';

await ffuzzyInitialize();
```

## 搜索模式

所有模式均接受可选的第二参数来覆盖语料默认选项。

```ts
corpus.fuzzy    ('src m')   // 子序列匹配 —— 支持 ! ^ ' $ 操作符
corpus.substring('src/')    // 连续子串
corpus.prefix   ('src/')    // 必须以 query 开头
corpus.postfix  ('.ts')     // 必须以 query 结尾
corpus.exact    ('main.ts') // 全串精确匹配
```

### 原始对象快捷方式（`*Raws`）

只需要命中 item、不需要 score/indices 等元数据时，`*Raws` 系列跳过 `FuzzyHit`
包装，速度更快：

```ts
const items: string[] = corpus.fuzzyRaws('src');
// 等价但更快于 corpus.fuzzy('src').map(h => h.raw)
```

可用方法：`fuzzyRaws` / `substringRaws` / `prefixRaws` / `postfixRaws` / `exactRaws`

## 选项

```ts
import { FuzzyCorpus, FuzzyCase, FuzzyNorm, FuzzyScoring } from '@codejoo/ffuzzy';

const corpus = new FuzzyCorpus(items, {
  stringOf: item => item.name,
  options: {
    caseMatching: FuzzyCase.smart,    // 0=区分大小写 1=不区分 2=智能（默认）
    normalization: FuzzyNorm.smart,   // 0=不归一 1=智能变音符归一（默认）
    limit: 50,                        // 最多返回数（0=全部）
    highlight: false,                 // true 时填充 FuzzyHit.indices（默认 false）
    scoring: FuzzyScoring.fast,       // fast（默认）/ off（不排名）/ nucleo（高精度）
  },
  matchPaths: false,   // 将 '/' 视为路径分隔符
  preferPrefix: false, // 偏向靠前的命中加分
});
```

单次调用覆盖：

```ts
corpus.fuzzy('query', { limit: 10, highlight: true });
```

## 多键搜索（拼音 / 罗马音）

```ts
import { FuzzyCorpus, FuzzyKey, FuzzyKeyKind } from '@codejoo/ffuzzy';

corpus.addKey(item, [
  FuzzyKey.kind('zhongguo', FuzzyKeyKind.pinyin),
  FuzzyKey.kind('zg',       FuzzyKeyKind.initials),
]);
```

> Map 语料：`FuzzyCorpus.byKey(rows, 'name')`（单字段）或
> `FuzzyCorpus.byKeys(rows, ['name', 'email'])`（多字段；`hit.matchedKey` 是字段下标）。
> 变更：`add` / `addAll` / `addKey` / `update` / `removeAt` / `removeWhere` / `refresh` / `clear`。

## 命中高亮

搜索时传 `{ highlight: true }` 才会填充 `FuzzyHit.indices`（默认 `false` 以节省
C 端 Pass 2 开销）。

**方式 A —— `highlightHtml`**（便利函数，内置 HTML 转义，防 XSS）：

```ts
import { highlightHtml } from '@codejoo/ffuzzy';

const [hit] = corpus.fuzzy('src', { highlight: true });
element.innerHTML = highlightHtml(hit.raw, hit.indices);
// → '<mark>src</mark>/main.dart'
// 自定义标签：highlightHtml(hit.raw, hit.indices, { tag: 'b' })
```

**方式 B —— 原始码点位置**（用于 Flutter 或自定义渲染）：

```ts
import { fuzzyCodepointToUtf16 } from '@codejoo/ffuzzy';

const [hit] = corpus.fuzzy('src', { highlight: true });
const u16 = fuzzyCodepointToUtf16(hit.raw, hit.indices);
// 将 u16 偏移量应用到 DOM Range / TextSpan / Highlight API
```

## `using` 语句

```ts
using corpus = FuzzyCorpus.strings(items); // 离开作用域自动 dispose
```

## FuzzyHit 结构

```ts
interface FuzzyHit<T> {
  raw:         T;        // 命中的原始对象
  index:       number;   // 在语料中的插入序号
  score:       number;   // 匹配分（越高越好，仅同一次查询内可比）
  matchedKind: number;   // 命中键的类型（FuzzyKeyKind）
  matchedKey:  number;   // 命中的是该 item 的第几个键
  indices:     number[]; // 命中的码点位置 —— 仅 highlight:true 时有值
}
```

## 从源码构建

```sh
# *.d.ts.src 是类型声明的可编辑源；修改后运行 build 重新生成 *.d.ts：
cd wasm && npm run build        # 拼接 wrapper → ffuzzy.js / ffuzzy-lite.js + 生成 *.d.ts

# 重建 WASM 引擎（需要 Emscripten ≥3.x）：
npm run build:engine            # emcc 编译 src/*.c → *.engine.mjs，然后自动 npm run build
```

## 相关

- [pub.dev 上的 ffuzzy](https://pub.dev/packages/ffuzzy) —— Flutter / Dart 包
- [GitHub](https://github.com/icodejoo/ffuzzy)

## 许可证

MIT

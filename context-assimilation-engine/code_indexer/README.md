# Code Indexer

Summarize -> embed -> search over a source tree, entirely in memory. This is
the MVP of the Code Indexer module planned under the Context Assimilation
Engine: given a directory of code it produces

- a short LLM summary per file (reusing the CAE Summarizer's inference path),
- embedding vectors for each summary and for line-window chunks of the code,
- a query API that ranks files for a natural-language question.

It is a **plain library plus a CLI, not yet a ChiMod/pool**: nothing here is
stored in the CTE, and nothing persists across process runs. Persistence,
tiering, summary evolution and a pool wrapper are deliberately later layers.

## Layout

| Path | What |
|---|---|
| `include/.../code_scanner.h` | directory walk + line-window chunking (std only) |
| `include/.../vector_index.h` | exact cosine `VectorIndex`, identifier-aware `Bm25Index` |
| `include/.../embed_client.h` | `Embedder` interface, `HttpEmbedder` (Ollama + OpenAI wire formats) |
| `include/.../code_indexer.h` | `CodeIndexer`: `IndexFiles()`, `Query()` |
| `include/.../ollama_summarizer.h` | `MakeOllamaSummarizer()` over `summarizer::GenerateSummary` |
| `util/clio_code_index.cc` | `clio_code_index` command-line tool |
| `test/test_code_indexer.cc` | unit tests (fake embedder + in-process HTTP stub) |

Two static libraries: `clio_cae_code_indexer` (pipeline; libcurl +
nlohmann_json only) and `clio_cae_code_indexer_summarize` (adds the CAE
summarizer runtime). Built only when `CAE_SUMMARIZATION_ENABLED`.

## Search modes

| Mode | Ranks files by |
|---|---|
| `summary` | cosine between the query and the file's summary vector |
| `code` | best cosine between the query and any code chunk of the file |
| `hybrid` | reciprocal-rank fusion (k=60) of `summary` and `code` |
| `keyword` | BM25 over file path + source; needs no model |

`summary` and `code` are separate on purpose: whether an LLM summary is a
better retrieval key than the code itself is an open question this module
must be able to measure.

## Embedding models

`HttpEmbedder` speaks two wire formats, chosen with `--embed-api`:

- `ollama`: `POST /api/embed` (Ollama >= 0.11.10).
- `openai`: `POST /v1/embeddings` (llama.cpp server, vLLM, LM Studio, ...).

Defaults follow EmbeddingGemma: query prefix `task: code retrieval | query: `,
document prefix `title: {title} | text: `, optional Matryoshka truncation
(`--embed-dim 768|512|256|128`, re-normalized). EmbeddingGemma 2
(`google/embeddinggemma-2`, 8192 tokens, Apache-2.0) documents
sentence-transformers as its runtime; the Ollama library listed only
EmbeddingGemma v1 (`embeddinggemma`, 2K context) when this was written, so
serve v2 through an OpenAI-compatible shim if Ollama lacks it.

## Building and testing

```bash
cmake --preset release            # CAE is on by default
cmake --build build -j --target test_code_indexer clio_code_index
ctest --test-dir build -R '^code_indexer$' --output-on-failure
```

## Command line

```bash
clio_code_index --root path/to/src --summ-model gemma3:1b \
    --embed-model embeddinggemma --mode all --top-k 5 \
    --query "where are blobs moved between storage tiers"
```

`--no-embed` runs keyword-only; omitting `--summ-model` indexes code without
summaries. Run with no arguments for the full option list.

## Known limits (MVP)

- In-memory only; the index is rebuilt every run.
- Exact brute-force search: fine for thousands of chunks, not millions.
- Files are summarized one at a time and embedded one file per request group;
  no parallelism.
- A file larger than the summarizer's context is chunked and the per-chunk
  summaries are concatenated, which can exceed the embedder's context. The
  embedder's own truncation applies.
- No summary evolution, retry loop, dependency extraction or file watcher.

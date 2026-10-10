# Code Indexer

Summarize -> embed -> search over a source tree, entirely in memory. This is
the MVP of the Code Indexer module planned under the Context Assimilation
Engine: given a directory of code it produces

- a short LLM summary per file (reusing the CAE Summarizer's inference path),
- embedding vectors for each summary and for line-window chunks of the code,
- a query API that ranks files for a natural-language question.

It ships three ways to use the same pipeline: a C++ library, a standalone
command-line tool (`clio_code_index`), and a **ChiMod pool**
(`clio_cae_code_indexer`) with a client CLI (`clio_code_index_client`) so it
can be composed into a CLIO runtime next to the CTE and Summarizer. Nothing is
stored in the CTE and nothing persists across process runs: persistence,
tiering and summary evolution are deliberately later layers.

## Layout

| Path | What |
|---|---|
| `include/.../code_scanner.h` | directory walk + line-window chunking (std only) |
| `include/.../vector_index.h` | exact cosine `VectorIndex`, identifier-aware `Bm25Index` |
| `include/.../embed_client.h` | `Embedder` interface, `HttpEmbedder` (Ollama + OpenAI wire formats) |
| `include/.../code_indexer.h` | `CodeIndexer`: `IndexFiles()`, `Query()` |
| `include/.../ollama_summarizer.h` | `MakeOllamaSummarizer()` over `summarizer::GenerateSummary` |
| `util/clio_code_index.cc` | `clio_code_index` standalone command-line tool |
| `clio_mod.yaml`, `include/.../code_indexer_{tasks,client,runtime}.h`, `src/code_indexer_runtime.cc`, `src/autogen/` | the ChiMod: methods `IndexPath` (10), `Query` (11), `Status` (12) |
| `util/clio_code_index_client.cc` | `clio_code_index_client`: talks to a running pool |
| `test/test_code_indexer.cc` | unit tests (fake embedder + in-process HTTP stub) |
| `test/test_code_indexer_pool.cc` | the pool through a live runtime (HTTP stub as the model) |

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

## Running it as a pool

Add a compose entry (pool id `402.0`; `400.0` is the CAE core, `401.0` the
Summarizer). Keys match the `CreateParams` fields:

```yaml
compose:
  - mod_name: clio_cae_code_indexer
    pool_name: code_indexer
    pool_query: local
    pool_id: "402.0"
    embed_endpoint: "http://127.0.0.1:11434"
    embed_model: "embeddinggemma"
    embed_api: "ollama"          # or "openai"
    embed_dim: 0                 # 768/512/256/128 = Matryoshka truncation
    summarize_endpoint: "http://127.0.0.1:11434"
    summarize_model: "gemma3:1b" # omit to index code without summaries
    summarize_ctx: 8192
    summarize_num_predict: 256
```

Then, with `CLIO_SERVER_CONF` pointing at that file and the runtime started
(`clio_run runtime start`):

```bash
clio_code_index_client index /path/to/src          # blocks until done
clio_code_index_client query "where are blobs reorganized" --mode hybrid
clio_code_index_client status
```

`PATH` is read on the node running the pool. `query` exits 3 when an
embedding was needed and the embedding server could not be reached (keyword
mode still works). An empty or unreadable `PATH` makes `index` fail.

Concurrency: the index is locked per file while indexing and per query, so a
query during a long `index` sees a partial index and waits at most one file.
`IndexPath` blocks one runtime worker for the whole directory (inference is
synchronous), so keep `runtime.num_threads` above 1.

## Building and testing

```bash
cmake --preset release            # CAE is on by default
cmake --build build -j --target test_code_indexer test_code_indexer_pool \
    clio_code_index clio_code_index_client
ctest --test-dir build -R 'code_indexer' --output-on-failure
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
- The pool's autogen (`src/autogen/code_indexer_lib_exec.cc`) is hand
  maintained: `clio_run repo refresh` currently emits a template that no
  longer matches `clio::run::Container` (see `../clio_repo.yaml`).
- Pool replicas are not coordinated: each container indexes only the paths it
  is asked to; queries merge by score (`QueryTask::AggregateOut`).

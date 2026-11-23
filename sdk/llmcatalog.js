// llmcatalog.js — the local language models Sentinela can use (docs/SENTINELA.md, "Models"), and the engine that runs
// them. Each model is a signed lordos-llm-<id> package of LordOS's APT repository, built by tools/llm/mkdeb-llm.mjs from
// the GGUF file below, which tools/llm/fetch-models.sh downloads from Hugging Face and checks against this SHA-256 (the
// same way the voices are made: tools/voice/). The package installs /usr/share/lordos/llm/<id>.gguf, the only folder
// lord-server takes models from (server/src/llm/engine.h). The engine is LordOS's own llama.cpp package (the Store's
// Artificial Intelligence catalog), built from a pinned upstream release; Debian 13 has none.
//
// Pure data (no `lord`), read by the Settings page, the Sentinela service, the packaging tools and the tests.

export const ENGINE_PACKAGE = 'llama.cpp';

export const LLM_MODELS = [
  {
    id: 'qwen3-1.7b', name: 'Qwen3 1.7B', quant: 'Q4_K_M', package: 'lordos-llm-qwen3-1.7b',
    source: 'ggml-org/Qwen3-1.7B-GGUF', file: 'Qwen3-1.7B-Q4_K_M.gguf', bytes: 1282439264,
    sha256: 'd2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5',
    license: 'Apache-2.0', ramMb: 2300, recommended: true,
  },
  {
    id: 'qwen3-4b', name: 'Qwen3 4B', quant: 'Q4_K_M', package: 'lordos-llm-qwen3-4b',
    source: 'ggml-org/Qwen3-4B-GGUF', file: 'Qwen3-4B-Q4_K_M.gguf', bytes: 2497280640,
    sha256: 'ab27b9bfa375a178d6cba48f3ad892b94b7739659dcc7aae8058ce0ffed6b328',
    license: 'Apache-2.0', ramMb: 4200, recommended: false,
  },
];

export const llmModel = (id) => LLM_MODELS.find((m) => m.id === id) || null;

// the packages Settings installs for a model: the engine first when it is missing
export function packagesForModel(id, engineInstalled) {
  const m = llmModel(id);
  if (!m) return [];
  return engineInstalled ? [m.package] : [ENGINE_PACKAGE, m.package];
}

// a package name the Settings page may ask pkg-install for: the engine or one of these models, nothing else
export const llmPackageOk = (name) => name === ENGINE_PACKAGE || LLM_MODELS.some((m) => m.package === name);

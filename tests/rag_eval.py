#!/usr/bin/env python3
"""RAG evaluation: component recall + optional LLM-as-Judge via DeepSeek flash.
Usage:
  python3 rag_eval.py --gt eval_queries_full.json
  python3 rag_eval.py --gt eval_queries_full.json --judge
"""

import json, os, subprocess, sys, re, time, argparse, urllib.request

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(os.environ.get("BINDIR", os.path.join(PROJECT_ROOT, "build/bin")), "fast-llm-cli")
MODEL = os.environ.get("MODEL", os.path.join(PROJECT_ROOT, "models/qwen2.5-3b-instruct-q4_k_m.gguf"))
EMBED_MODEL = os.environ.get("EMBEDDING_MODEL", os.path.join(PROJECT_ROOT, "models/bge-small-zh-v1.5-q4_k_m.gguf"))
TIMEOUT = int(os.environ.get("TIMEOUT", "300"))
API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
API_BASE = os.environ.get("DEEPSEEK_API_BASE", "https://api.deepseek.com")
JUDGE_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-v4-flash")


def p_at_k(retrieved, relevant, k):
    hits = sum(1 for r in retrieved[:k] if r in relevant)
    return hits / k if k > 0 else 0


def run_cli(queries):
    cmds = ""
    for q in queries:
        cmds += f"/vec {q['query']}\n/bm5 {q['query']}\n/search {q['query']}\n/parents {q['query']}\n"
    cmds += "/quit\n"

    print(f"Running {len(queries)} queries ...", end=" ", flush=True)
    t0 = time.time()
    proc = subprocess.run(
        [CLI, "--model", MODEL, "--embedding-model", EMBED_MODEL],
        input=cmds.encode("utf-8"), capture_output=True, timeout=TIMEOUT,
        env={**os.environ, "LC_ALL": "en_US.UTF-8", "LANG": "en_US.UTF-8"}
    )
    output = proc.stdout.decode("utf-8", errors="replace") + proc.stderr.decode("utf-8", errors="replace")
    print(f"done ({time.time()-t0:.1f}s)")
    return output


def parse_results(output):
    search_re = re.compile(r"^\s{2}\[(\d+)\]\s+\S+/(.+?)\s+\S+\s+score=([0-9.]+)\s+doc=(\d+)")
    parent_re = re.compile(r"^\s{2}\[(\d+)\]\s+\S+/(.+?)\s+\S+\s+matches=\d+\s+score=([0-9.]+)\s+doc=(\d+)")

    qi, mode = 0, 0
    rank1_seen = False
    results = {"vec": {}, "bm5": {}, "hybrid": {}, "parents": {}}

    for line in output.splitlines():
        m = parent_re.match(line)
        if m:
            mode = 3
            dish = m.group(2).strip()
            results["parents"].setdefault(qi, []).append(dish)
            continue
        m = search_re.match(line)
        if m:
            if m.group(1) == "1":
                if mode == 3:
                    qi += 1; mode = 0; rank1_seen = True
                elif not rank1_seen:
                    rank1_seen = True
                else:
                    mode += 1
            dish = m.group(2).strip()
            if mode == 0: results["vec"].setdefault(qi, []).append(dish)
            elif mode == 1: results["bm5"].setdefault(qi, []).append(dish)
            elif mode == 2: results["hybrid"].setdefault(qi, []).append(dish)
    return results


def judge_retrieval(query, chunks, doc_map):
    """Batch judge: send query + retrieved chunks to flash, get relevance labels."""
    system = ("Determine which retrieved chunks are relevant to the query. "
              "Output ONLY a JSON array of chunk numbers, e.g. [1, 3]. If none, output [].")

    user = f"Query: {query}\n\nRetrieved chunks:\n"
    for i, (dish, cat) in enumerate(chunks, 1):
        info = doc_map.get(dish, {})
        content = info.get("content", "")[:400]
        user += f"[{i}] {dish} [{cat}]: {content}\n"

    body = json.dumps({
        "model": JUDGE_MODEL,
        "messages": [{"role": "system", "content": system},
                     {"role": "user", "content": user}],
        "max_tokens": 16384, "temperature": 0
    }).encode("utf-8")

    try:
        req = urllib.request.Request(f"{API_BASE.rstrip('/')}/chat/completions", data=body, headers={
            "Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json"
        })
        with urllib.request.urlopen(req, timeout=60) as resp:
            answer = json.loads(resp.read())["choices"][0]["message"]["content"]
            m = re.search(r'\[[\d,\s]*\]', answer)
            return json.loads(m.group()) if m else []
    except Exception as e:
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gt", required=True, help="Ground truth JSON")
    parser.add_argument("--judge", action="store_true", help="Run LLM-as-Judge")
    parser.add_argument("--top-k", type=int, default=3)
    args = parser.parse_args()

    with open(args.gt) as f:
        queries = json.load(f)

    if not os.path.isfile(CLI):
        print(f"ERROR: CLI not found at {CLI}"); sys.exit(1)

    # Load doc map for judge
    doc_map = {}
    bm25_file = os.path.join(PROJECT_ROOT, "data", "rag", "bm25.json")
    if os.path.isfile(bm25_file):
        with open(bm25_file) as f:
            for e in json.load(f):
                dish = e.get("dish", "")
                if dish and dish not in doc_map:
                    doc_map[dish] = {"category": e.get("category", "?"), "content": e.get("content", "")}

    output = run_cli(queries)
    results = parse_results(output)

    modes = ["vec", "bm5", "hybrid", "parents"]

    # Recall report
    H = f"{'Query':28s} {'Vec@3':>7s} {'BM25@3':>7s} {'Hyb@3':>7s} {'Par@3':>7s}"
    print("\n" + "=" * len(H))
    print(H)
    print("=" * len(H))

    sums = {m: 0.0 for m in modes}
    judge_sums = {m: 0.0 for m in modes}
    judge_total = 0

    for qi, q in enumerate(queries):
        rel = set(q["relevant_docs"])
        qs = q["query"][:27]
        parts = []
        for m in modes:
            ret = results[m].get(qi, [])[:args.top_k]
            p = p_at_k(ret, rel, args.top_k)
            sums[m] += p
            parts.append(f"{p:7.4f}")
        print(f"{qs:28s} {' '.join(parts)}")

    n = len(queries)
    print("=" * len(H))
    avg_parts = [f"{sums[m]/n:7.4f}" for m in modes]
    print(f"{'AVERAGE':28s} {' '.join(avg_parts)}")
    print()

    # Judge
    if args.judge and API_KEY:
        print("=== LLM-as-Judge (Hybrid@3) ===")
        hdr = f"{'Query':28s} {'GT':>4s} {'Judge':>6s} {'Agree':>6s}"
        print(hdr); print("-" * len(hdr))

        gt_yes = judge_yes = agree = total = 0
        for qi, q in enumerate(queries):
            rel = set(q["relevant_docs"])
            hyb = results["hybrid"].get(qi, [])[:args.top_k]
            # Collect metadata for chunks
            chunks = []
            for dish in hyb:
                cat = doc_map.get(dish, {}).get("category", "?")
                chunks.append((dish, cat))

            labels = judge_retrieval(q["query"], chunks, doc_map)
            if labels is None:
                continue

            for i, (dish, _) in enumerate(chunks, 1):
                gt = dish in rel
                jd = i in labels
                m = "✓" if gt == jd else "✗"
                if gt: gt_yes += 1
                if jd: judge_yes += 1
                if gt == jd: agree += 1
                total += 1

            jd_count = sum(1 for i in range(1, len(chunks)+1) if i in labels)
            jd_match = sum(1 for i, (d, _) in enumerate(chunks, 1)
                          if (d in rel) == (i in labels))
            print(f"{q['query'][:27]:28s} {len(rel & set(d for d,_ in chunks))}/{len(chunks):>3} "
                  f"{jd_count}/{len(chunks):>3} {jd_match}/{len(chunks):>6}")

        print("-" * len(hdr))
        print(f"{'TOTAL':28s} {gt_yes}/{total:>3} {judge_yes}/{total:>3} {agree}/{total:>6}")
        print()

    print("Done.")


if __name__ == "__main__":
    main()

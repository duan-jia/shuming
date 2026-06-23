#!/usr/bin/env python3
"""GT Generator: refresh existing queries or auto-generate new ones via DeepSeek flash.
Usage:
  python3 gt_gen.py --queries eval_queries.json --output eval_queries_full.json
  python3 gt_gen.py --gen --categories soup,meat_dish,... --output new.json
"""

import json, os, sys, re, argparse, urllib.request

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS_FILE = os.path.join(PROJECT_ROOT, "data", "rag", "bm25.json")
API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
API_BASE = os.environ.get("DEEPSEEK_API_BASE", "https://api.deepseek.com")
MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-v4-flash")


def load_docs():
    with open(DOCS_FILE) as f:
        data = json.load(f)
    docs = {}
    for e in data:
        dish = e["dish"]
        if dish not in docs:
            docs[dish] = {"category": e.get("category", "general"), "content": e.get("content", "")}
    return docs


def call_flash(system, user):
    body = json.dumps({
        "model": MODEL,
        "messages": [{"role": "system", "content": system},
                     {"role": "user", "content": user}],
        "max_tokens": 16384, "temperature": 0
    }).encode("utf-8")
    req = urllib.request.Request(f"{API_BASE.rstrip('/')}/chat/completions", data=body, headers={
        "Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json"
    })
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read())["choices"][0]["message"]["content"].strip()


def parse_json_array(text):
    m = re.search(r'\[[\d,\s]*\]', text)
    return json.loads(m.group()) if m else []


def refresh_gt(queries, docs, output_path):
    """Batch each query against all docs, get relevant dish numbers via flash."""
    doc_list = sorted(docs.items(), key=lambda x: x[0])  # [(dish, {category, content}), ...]

    system = ("Identify documents relevant to the query. "
              "Output ONLY a JSON array of numbers, e.g. [1, 3, 5]. If none, output [].")

    new_queries = []
    for qi, q in enumerate(queries):
        user = f"Query: {q['query']}\n\nDocuments:\n"
        for i, (dish, info) in enumerate(doc_list, 1):
            user += f"[{i}] {dish} [{info['category']}]: {info['content'][:300]}\n"

        print(f"  [{qi+1}/{len(queries)}] {q['query'][:35]} ...", end=" ", flush=True)
        try:
            answer = call_flash(system, user)
            nums = parse_json_array(answer)
            relevant = [doc_list[n-1][0] for n in nums if 1 <= n <= len(doc_list)]
            new_queries.append({"query": q["query"], "relevant_docs": sorted(relevant)})
            print(f"GT={len(relevant)} {'✓' if relevant else ''}")
        except Exception as e:
            print(f"ERROR: {e} — keeping old GT")
            new_queries.append(q)

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(new_queries, f, ensure_ascii=False, indent=2)
    print(f"Written {len(new_queries)} queries → {output_path}")


def gen_queries(category_names, docs, output_path):
    """For each category, have flash generate 1-2 queries + relevant dishes."""
    docs_by_cat = {}
    for dish, info in docs.items():
        docs_by_cat.setdefault(info["category"], []).append(dish)

    cats = [c for c in category_names if c in docs_by_cat]
    if not cats:
        cats = sorted(docs_by_cat.keys())

    system = ("Generate realistic search queries for a recipe knowledge base. "
              "For the given category and dish list, output 1-2 short Chinese queries "
              "a user might ask, with the dish numbers that are relevant. "
              "Format as JSON: [{\"query\": \"...\", \"relevant\": [1, 5]}, ...]")

    all_queries = []
    for cat in cats:
        dishes = docs_by_cat[cat]
        user = f"Category: {cat}\nDishes:\n"
        for i, dish in enumerate(dishes, 1):
            user += f"[{i}] {dish}\n"
        user += "\nGenerate queries as JSON array."

        print(f"  [{cat}] {len(dishes)} dishes ...", end=" ", flush=True)
        try:
            answer = call_flash(system, user)
            arr = re.search(r'\[.*\]', answer, re.DOTALL)
            if arr:
                items = json.loads(arr.group())
                for item in items:
                    rel = [dishes[n-1] for n in item.get("relevant", []) if 1 <= n <= len(dishes)]
                    if item.get("query") and rel:
                        all_queries.append({"query": item["query"], "relevant_docs": sorted(rel)})
                        print(f"+{item['query'][:30]}")
        except Exception as e:
            print(f"ERROR: {e}")

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(all_queries, f, ensure_ascii=False, indent=2)
    print(f"Written {len(all_queries)} queries → {output_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--queries", default=None, help="Input queries JSON to refresh GT")
    parser.add_argument("--gen", action="store_true", help="Auto-generate queries")
    parser.add_argument("--categories", default="", help="Comma-separated categories for --gen")
    parser.add_argument("--output", required=True, help="Output JSON file")
    args = parser.parse_args()

    if not API_KEY:
        print("ERROR: set DEEPSEEK_API_KEY")
        sys.exit(1)
    docs = load_docs()
    print(f"Loaded {len(docs)} documents, model={MODEL}")

    if args.gen:
        cats = [c.strip() for c in args.categories.split(",") if c.strip()]
        gen_queries(cats, docs, args.output)
    elif args.queries:
        with open(args.queries) as f:
            queries = json.load(f)
        refresh_gt(queries, docs, args.output)
    else:
        print("ERROR: need --queries or --gen")
        sys.exit(1)


if __name__ == "__main__":
    main()

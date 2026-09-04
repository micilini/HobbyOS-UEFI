#!/usr/bin/env python3
import argparse
from pathlib import Path

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",default="kernel")
    ap.add_argument("--limit",type=int,default=2048)
    ap.add_argument("--report",default="artifacts/build/stack-usage-report.txt")
    args=ap.parse_args()
    rows=[]; bad=[]
    files=list(Path(args.root).rglob("*.su"))
    for path in files:
        for line in path.read_text(errors="replace").splitlines():
            parts=line.split("\t")
            if len(parts)<3: continue
            try: size=int(parts[1])
            except ValueError: continue
            kind=parts[2].strip(); row=(size,kind,parts[0],str(path))
            rows.append(row)
            if size>args.limit or "unbounded" in kind: bad.append(row)
    rows.sort(reverse=True)
    lines=[f"STACK_USAGE limit={args.limit} files={len(files)}","TOP 20"]
    lines += [f"{size:5d} {kind:16s} {name} [{path}]" for size,kind,name,path in rows[:20]]
    lines.append(f"violations={len(bad)}")
    out=Path(args.report); out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text("\n".join(lines)+"\n"); print("\n".join(lines))
    if not rows: print("stack-check: FAIL no .su files"); return 2
    if bad:
        for size,kind,name,path in bad: print(f"VIOLATION {size} {kind} {name} [{path}]")
        print("stack-check: FAIL"); return 1
    print("stack-check: PASS"); return 0

if __name__=="__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""通过 GitHub REST API 创建公开仓库并上传 FlashLaunch 源码（无需 git/gh）。

用法：
    python3 tools/upload_to_github.py <用户名> <token>

token 需要 "Contents: read and write" 与 "Metadata: read" 权限（细粒度 token），
或 classic token 勾选 repo/public_repo。token 只在本脚本运行时使用，不写入仓库。

上传的文件清单见 FILES；.sysroot/.apt/.tools/dist 等工具链与产物不会上传。
"""
import sys
import json
import base64
import urllib.request
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "FlashLaunch"
DESCRIPTION = "Windows 原生轻量快速启动器：拼音/中文搜索、全局热键、计算器、多套主题，单文件绿色 exe"

# 要上传的文件（相对项目根目录）
FILES = [
    "README.md",
    "LICENSE",
    ".gitignore",
    "build.sh",
    "package.sh",
    "src/FlashLaunch.cpp",
    "src/pinyin_table.h",
    "src/resource.rc",
    "src/app.ico",
    "tools/gen_pinyin.py",
    "tools/gen_icon.py",
    "tools/upload_to_github.py",
]

API = "https://api.github.com"


def api(method, path, token, payload=None):
    url = API + path
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("Authorization", f"Bearer {token}")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        return e.code, {"message": body}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    owner, token = sys.argv[1], sys.argv[2]

    # 1) 确认身份
    status, user = api("GET", "/user", token)
    if status != 200:
        print(f"[错误] token 无效或权限不足 (HTTP {status}): {user.get('message')}")
        sys.exit(1)
    login = user.get("login")
    print(f"[OK] 已认证为 {login}")

    # 2) 先读取仓库；不存在时再尝试创建（细粒度 token 无建仓权限，需预先在网页建好）
    status, repo = api("GET", f"/repos/{owner}/{REPO}", token)
    if status == 200:
        print(f"[OK] 复用已有仓库: {repo['html_url']}")
    else:
        status, repo = api("POST", "/user/repos", token, {
            "name": REPO,
            "description": DESCRIPTION,
            "public": True,
            "has_issues": True,
            "has_wiki": False,
        })
        if status == 201:
            print(f"[OK] 仓库已创建: {repo['html_url']}")
        else:
            print(f"[错误] 仓库不存在且无法创建 (HTTP {status}): {repo.get('message')}")
            print("       请先在 https://github.com/new 手动建一个同名空仓库（Public，不勾选 README/.gitignore/license）")
            sys.exit(1)

    branch = repo.get("default_branch", "main")

    # 3) 逐个上传文件（contents API，每个文件一次 commit）
    for rel in FILES:
        path = ROOT / rel
        if not path.exists():
            print(f"[跳过] 文件不存在: {rel}")
            continue
        content = base64.b64encode(path.read_bytes()).decode()
        payload = {
            "message": f"Add {rel}" if rel == FILES[0] else f"Update {rel}",
            "content": content,
            "branch": branch,
        }
        status, result = api("PUT", f"/repos/{owner}/{REPO}/contents/{rel}", token, payload)
        if status in (200, 201):
            print(f"[OK] 上传 {rel}")
        else:
            print(f"[错误] 上传 {rel} 失败 (HTTP {status}): {result.get('message')}")
            sys.exit(1)

    print(f"\n[完成] {repo['html_url']}")


if __name__ == "__main__":
    main()

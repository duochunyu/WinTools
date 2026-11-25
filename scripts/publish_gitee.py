#!/usr/bin/env python
"""把 GitHub 上的某个发布同步到 Gitee 镜像仓，一条命令跑完全部三步。

    python scripts/publish_gitee.py v0.3.2

三步：建 Gitee release、传附件、改镜像仓的 update.txt。全部幂等，重跑不会产生重复。

为什么需要它：GITEE_TOKEN 目前没有配成 GitHub secret，所以 GitHub Actions 只发 GitHub 一半。
配好 secret 之后这个脚本就可以退休了——那时流水线自己会建 release、传附件、推 feed。

几个必须记住的约束（都踩过）：
  * 附件名必须正好是 WinTools.exe。feed 里的下载地址是拼出来的，名字不对就是 404。
  * Gitee 的 release 接口不返回附件 id，删不掉单个附件，名字不对只能整个 release 重建。
  * DELETE release 返回 500 但实际已经删掉，别被状态码骗了。
  * 镜像仓的 feed 主地址指向 Gitee 自己的资产；GitHub 那份指向 GitHub。
    两边 version / size / sha256 必须一致，字节可以不同。
  * token 只从 ~/.workbuddy/secrets/gitee-token.txt 读，不进命令行、不进仓库。
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

GIT_OWNER = "duochunyu"
GIT_REPO = "WinTools"
MIRROR_OWNER = "YingKeWangLuo_admin"
MIRROR_REPO = "main"
ASSET_NAME = "WinTools.exe"
GITEE = "https://gitee.com/api/v5"
TOKEN_FILE = os.path.expanduser(r"~\.workbuddy\secrets\gitee-token.txt")
DEFAULT_PROXY = "http://192.168.2.9:31181"


def proxy_for_github():
    for name in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        value = os.environ.get(name)
        if value and "127.0.0.1" not in value and "localhost" not in value:
            return value
    return DEFAULT_PROXY


def openers():
    """GitHub 要走代理，Gitee 走直连：Gitee 是国内站点，经代理反而慢或者不通。"""
    proxy = proxy_for_github()
    github = urllib.request.build_opener(
        urllib.request.ProxyHandler({"http": proxy, "https": proxy})
    )
    github.addheaders = [("User-Agent", "WinTools-publish")]
    gitee = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    return github, gitee


GITHUB_OPENER, GITEE_OPENER = openers()


def token():
    try:
        with open(TOKEN_FILE, "r", encoding="utf-8") as handle:
            value = handle.read().strip()
    except FileNotFoundError:
        raise SystemExit("缺少 token 文件：%s" % TOKEN_FILE)
    if not value:
        raise SystemExit("token 文件是空的：%s" % TOKEN_FILE)
    return value


def gitee_call(method, path, body=None, content_type="application/x-www-form-urlencoded"):
    url = GITEE + path + "?" + urllib.parse.urlencode({"access_token": token()})
    request = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        request.add_header("Content-Type", content_type)
    try:
        with GITEE_OPENER.open(request, timeout=90) as response:
            raw = response.read().decode("utf-8", "replace")
            return response.status, (json.loads(raw) if raw.strip().startswith(("{", "[")) else raw)
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace")[:400]


def release_path(tag=""):
    base = "/repos/%s/%s/releases" % (MIRROR_OWNER, MIRROR_REPO)
    return base if not tag else base + "/tags/" + tag


def github_release(tag):
    url = "https://api.github.com/repos/%s/%s/releases/tags/%s" % (GIT_OWNER, GIT_REPO, tag)
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    try:
        with GITHUB_OPENER.open(request, timeout=60) as response:
            data = json.load(response)
    except urllib.error.HTTPError as error:
        raise SystemExit("GitHub 上没有 %s 这个发布（%s）" % (tag, error.code))
    for asset in data.get("assets", []):
        if asset["name"] == ASSET_NAME:
            return data, asset
    raise SystemExit("GitHub 发布里没有 %s 附件" % ASSET_NAME)


def download(url, target):
    with GITHUB_OPENER.open(urllib.request.Request(url, headers={"Accept": "*/*"}), timeout=300) as response:
        payload = response.read()
    with open(target, "wb") as handle:
        handle.write(payload)
    return hashlib.sha256(payload).hexdigest(), len(payload)


def upload(release_id, path):
    boundary = "----wintoolsboundary"
    with open(path, "rb") as handle:
        payload = handle.read()
    head = (
        "--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{n}\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n".format(b=boundary, n=ASSET_NAME)
    ).encode()
    tail = "\r\n--{b}--\r\n".format(b=boundary).encode()
    return gitee_call(
        "POST",
        "/repos/%s/%s/releases/%s/attach_files" % (MIRROR_OWNER, MIRROR_REPO, release_id),
        body=head + payload + tail,
        content_type="multipart/form-data; boundary=" + boundary,
    )


def ensure_release(tag, version, digest, github_url):
    status, body = gitee_call("GET", release_path(tag))
    if status == 200 and isinstance(body, dict):
        release_id = body["id"]
        exes = [a["name"] for a in body.get("assets", []) if a.get("name", "").lower().endswith(".exe")]
        if exes == [ASSET_NAME]:
            print("release 已存在，附件名正确（id=%s）" % release_id)
            return release_id, False
        print("附件名不对 %s，整个 release 重建" % exes)
        status, _ = gitee_call("DELETE", "/repos/%s/%s/releases/%s" % (MIRROR_OWNER, MIRROR_REPO, release_id))
        print("  删除 release 返回 %s（500 是假错，实际已删）" % status)

    status, body = gitee_call("POST", release_path(), body=urllib.parse.urlencode({
        "tag_name": tag,
        "name": "WinTools " + version,
        "body": "WinTools %s\n\nSHA-256: %s\n\nGitHub 同步发布：%s" % (version, digest, github_url),
        "target_commitish": "main",
        "prerelease": "false",
    }).encode())
    if status not in (200, 201):
        raise SystemExit("建 release 失败 %s：%s\n（提示：镜像仓上可能还没有 %s 这个 tag，"
                         "需要先 git push 该 tag）" % (status, body, tag))
    print("release 已创建（id=%s）" % body["id"])
    return body["id"], True


def update_feed(tag, version, size, digest):
    feed = "\n".join([
        "schema=1",
        "version=" + version,
        "size=" + str(size),
        "sha256=" + digest,
        "url=https://gitee.com/%s/%s/releases/download/%s/%s" % (MIRROR_OWNER, MIRROR_REPO, tag, ASSET_NAME),
        "urlGithub=https://github.com/%s/%s/releases/download/%s/%s" % (GIT_OWNER, GIT_REPO, tag, ASSET_NAME),
        "notes.zh=修复问题并改进稳定性，详情见 Release 页面。",
        "notes.en=Bug fixes and stability improvements; see the release page for details.",
    ]) + "\n"

    path = "/repos/%s/%s/contents/update.txt" % (MIRROR_OWNER, MIRROR_REPO)
    status, body = gitee_call("GET", path)
    if status != 200:
        raise SystemExit("读取镜像仓 update.txt 失败 %s：%s" % (status, body))
    import base64
    if base64.b64decode(body["content"]).decode("utf-8", "replace") == feed:
        print("镜像仓 update.txt 已是最新，未改动")
        return
    # Form-encoded, not JSON: Gitee's contents API reads these four as form fields, and a JSON
    # body comes back as "content is missing, sha is missing, message is missing".
    payload = urllib.parse.urlencode({
        "content": base64.b64encode(feed.encode("utf-8")).decode("ascii"),
        "sha": body["sha"],
        "message": "chore: publish update.txt for " + tag,
        "branch": "main",
    }).encode()
    status, body = gitee_call("PUT", path, body=payload)
    if status not in (200, 201):
        raise SystemExit("更新 update.txt 失败 %s：%s" % (status, body))
    print("镜像仓 update.txt 已更新")


def main():
    parser = argparse.ArgumentParser(description="把 GitHub 发布同步到 Gitee 镜像仓")
    parser.add_argument("tag", help="例如 v0.3.2")
    parser.add_argument("--asset", help="不下载，直接用本地这份 exe")
    parser.add_argument("--skip-verify", action="store_true", help="跳过从 Gitee 回读校验")
    parser.add_argument("--skip-feed", action="store_true",
                        help="只发 release 和附件，不动镜像仓的 update.txt（补发旧版本时用）")
    args = parser.parse_args()

    tag = args.tag
    version = tag.lstrip("v")
    print("=== %s -> gitee.com/%s/%s ===" % (tag, MIRROR_OWNER, MIRROR_REPO))

    release, asset = github_release(tag)
    github_url = release["html_url"]

    if args.asset:
        with open(args.asset, "rb") as handle:
            payload = handle.read()
        digest = hashlib.sha256(payload).hexdigest()
        size = len(payload)
        local = args.asset
    else:
        target = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".workbuddy", "tmp")
        os.makedirs(target, exist_ok=True)
        local = os.path.join(target, "publish-%s.exe" % tag)
        print("从 GitHub 下载资产...")
        digest, size = download(asset["browser_download_url"], local)
    print("size=%d sha256=%s" % (size, digest))
    if size != asset["size"]:
        raise SystemExit("下载到的字节数与 GitHub 记录不一致：%d vs %d" % (size, asset["size"]))

    release_id, created = ensure_release(tag, version, digest, github_url)
    if created:
        status, body = upload(release_id, local)
        if status not in (200, 201):
            raise SystemExit("上传附件失败 %s：%s" % (status, body))
        print("附件已上传（%s）" % body.get("name"))

    status, body = gitee_call("GET", release_path(tag))
    names = [a["name"] for a in body.get("assets", [])] if isinstance(body, dict) else []
    print("release 附件：%s" % names)
    if ASSET_NAME not in names:
        raise SystemExit("附件名仍然不对：%s" % names)

    if not args.skip_verify:
        url = "https://gitee.com/%s/%s/releases/download/%s/%s" % (MIRROR_OWNER, MIRROR_REPO, tag, ASSET_NAME)
        check = local + ".gitee"
        print("从 Gitee 回读校验...")
        got, got_size = download(url, check)
        os.remove(check)
        if got != digest or got_size != size:
            raise SystemExit("Gitee 上的字节与 GitHub 不一致：%s/%d vs %s/%d" % (got, got_size, digest, size))
        print("回读一致：%s（%d 字节）" % (got, got_size))

    if args.skip_feed:
        print("\n--skip-feed：镜像仓 update.txt 保持不动")
    else:
        update_feed(tag, version, size, digest)
    print("\n完成。Gitee: https://gitee.com/%s/%s/releases/tag/%s" % (MIRROR_OWNER, MIRROR_REPO, tag))


if __name__ == "__main__":
    sys.exit(main())

import { stat } from "node:fs/promises";
import { join, normalize, resolve, extname } from "node:path";

const ROOT = resolve(process.env.OTA_ROOT ?? "./repo");
const PORT = Number(process.env.OTA_PORT ?? 8080);
const HOST = process.env.OTA_HOST ?? "0.0.0.0";

const TYPES: Record<string, string> = {
  ".toml": "text/plain; charset=utf-8",
  ".sig": "text/plain; charset=utf-8",
  ".grain": "application/octet-stream",
  ".pub": "text/plain; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
  ".json": "application/json; charset=utf-8",
};

function safePath(urlPath: string): string | null {
  const decoded = decodeURIComponent(urlPath);
  const clean = normalize(decoded).replace(/^(\.\.(\/|\\|$))+/, "");
  const full = join(ROOT, clean);
  if (!resolve(full).startsWith(ROOT)) return null;
  return full;
}

const tls =
  process.env.OTA_TLS_CERT && process.env.OTA_TLS_KEY
    ? {
        cert: Bun.file(process.env.OTA_TLS_CERT),
        key: Bun.file(process.env.OTA_TLS_KEY),
      }
    : undefined;

const server = Bun.serve({
  hostname: HOST,
  port: PORT,
  tls,
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/healthz") {
      return new Response("ok\n", { status: 200 });
    }
    const path = safePath(url.pathname);
    if (!path) return new Response("forbidden\n", { status: 403 });

    let target = path;
    try {
      const s = await stat(target);
      if (s.isDirectory()) target = join(target, "index.toml");
    } catch {
      return new Response("not found\n", { status: 404 });
    }

    const file = Bun.file(target);
    if (!(await file.exists())) return new Response("not found\n", { status: 404 });

    const ct = TYPES[extname(target)] ?? "application/octet-stream";
    return new Response(file, {
      headers: {
        "content-type": ct,
        "cache-control": "no-cache",
      },
    });
  },
});

console.log(`saltOS OTA server: serving ${ROOT} on ${tls ? "https" : "http"}://${HOST}:${server.port}`);

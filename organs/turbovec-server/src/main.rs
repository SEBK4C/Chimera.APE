//! turbovec.ape — the vector organ of Chimera.
//!
//! A deliberately tiny HTTP/1.1 server over `turbovec::IdMapIndex`, speaking
//! JSON on loopback to exactly one client: the chimera orchestrator. One
//! request at a time, `Connection: close`, no TLS, no async runtime.
//!
//! Endpoints:
//!   GET  /health   -> "ok"
//!   GET  /info     -> {"dim":3840|null,"count":N,"bit_width":4}
//!   POST /upsert   -> {"ids":[u64...],"vectors":[[f32...]...]}        -> {"upserted":N}
//!   POST /query    -> {"vectors":[[f32...]...],"k":24,"allowlist":[..]?}
//!                     -> {"scores":[[f32...]...],"ids":[[u64...]...]}
//!   POST /remove   -> {"ids":[u64...]}                                -> {"removed":N}
//!   POST /persist  -> {}                                              -> {"persisted":true,"count":N}
//!   POST /shutdown -> persist + exit(0)
//!
//! Usage:
//!   turbovec-server --port P --path index.tvim [--bit-width 4] [--dim N]
//!
//! With --port 0 an ephemeral port is chosen; the bound port is announced on
//! stdout as a single line `PORT <n>` either way, so the orchestrator can
//! always parse it.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::process::exit;

use serde::Deserialize;
use turbovec::IdMapIndex;

struct Args {
    port: u16,
    path: PathBuf,
    bit_width: usize,
    dim: Option<usize>,
}

fn parse_args() -> Args {
    let mut port = None;
    let mut path = None;
    let mut bit_width = 4usize;
    let mut dim = None;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut val = |name: &str| {
            it.next()
                .unwrap_or_else(|| die(&format!("missing value for {name}")))
        };
        match a.as_str() {
            "--port" => port = Some(val("--port").parse().unwrap_or_else(|_| die("bad --port"))),
            "--path" => path = Some(PathBuf::from(val("--path"))),
            "--bit-width" => {
                bit_width = val("--bit-width").parse().unwrap_or_else(|_| die("bad --bit-width"))
            }
            "--dim" => dim = Some(val("--dim").parse().unwrap_or_else(|_| die("bad --dim"))),
            "--help" | "-h" => {
                println!("turbovec-server --port P --path index.tvim [--bit-width 2|3|4] [--dim N]");
                exit(0);
            }
            other => die(&format!("unknown flag {other}")),
        }
    }
    Args {
        port: port.unwrap_or_else(|| die("--port is required")),
        path: path.unwrap_or_else(|| die("--path is required")),
        bit_width,
        dim,
    }
}

fn die(msg: &str) -> ! {
    eprintln!("turbovec-server: {msg}");
    exit(2);
}

#[derive(Deserialize)]
struct UpsertReq {
    ids: Vec<u64>,
    vectors: Vec<Vec<f32>>,
}

#[derive(Deserialize)]
struct QueryReq {
    vectors: Vec<Vec<f32>>,
    k: usize,
    #[serde(default)]
    allowlist: Option<Vec<u64>>,
}

#[derive(Deserialize)]
struct RemoveReq {
    ids: Vec<u64>,
}

struct Server {
    index: IdMapIndex,
    path: PathBuf,
}

impl Server {
    fn flatten_checked(&self, vectors: &[Vec<f32>]) -> Result<(Vec<f32>, usize), String> {
        let dim = match self.index.dim_opt() {
            Some(d) => d,
            None => vectors
                .first()
                .map(|v| v.len())
                .ok_or_else(|| "empty vector list".to_string())?,
        };
        let mut flat = Vec::with_capacity(vectors.len() * dim);
        for (i, v) in vectors.iter().enumerate() {
            if v.len() != dim {
                return Err(format!("vector {i} has dim {} (index dim {dim})", v.len()));
            }
            flat.extend_from_slice(v);
        }
        Ok((flat, dim))
    }

    fn upsert(&mut self, req: UpsertReq) -> Result<serde_json::Value, String> {
        if req.ids.len() != req.vectors.len() {
            return Err(format!(
                "ids ({}) and vectors ({}) length mismatch",
                req.ids.len(),
                req.vectors.len()
            ));
        }
        if req.ids.is_empty() {
            return Ok(serde_json::json!({"upserted": 0}));
        }
        // Within-batch duplicates: last occurrence wins, matching upsert
        // semantics; add_with_ids would reject duplicate ids outright.
        let mut keep: Vec<usize> = Vec::with_capacity(req.ids.len());
        {
            let mut seen = std::collections::HashSet::with_capacity(req.ids.len());
            for i in (0..req.ids.len()).rev() {
                if seen.insert(req.ids[i]) {
                    keep.push(i);
                }
            }
            keep.reverse();
        }
        let ids: Vec<u64> = keep.iter().map(|&i| req.ids[i]).collect();
        let vectors: Vec<Vec<f32>> = keep.into_iter().map(|i| req.vectors[i].clone()).collect();
        let (flat, dim) = self.flatten_checked(&vectors)?;
        for id in &ids {
            self.index.remove(*id);
        }
        // _2d locks the dim on a lazy index's first add; plain add_with_ids
        // panics there.
        self.index
            .add_with_ids_2d(&flat, dim, &ids)
            .map_err(|e| format!("add_with_ids_2d: {e:?}"))?;
        Ok(serde_json::json!({"upserted": ids.len()}))
    }

    fn query(&self, req: QueryReq) -> Result<serde_json::Value, String> {
        if req.vectors.is_empty() || req.k == 0 {
            return Ok(serde_json::json!({"scores": [[]], "ids": [[]]}));
        }
        if self.index.is_empty() {
            let empty: Vec<Vec<f32>> = req.vectors.iter().map(|_| Vec::new()).collect();
            let empty_ids: Vec<Vec<u64>> = req.vectors.iter().map(|_| Vec::new()).collect();
            return Ok(serde_json::json!({"scores": empty, "ids": empty_ids}));
        }
        let (flat, _dim) = self.flatten_checked(&req.vectors)?;
        let (scores, ids) =
            self.index
                .search_with_allowlist(&flat, req.k, req.allowlist.as_deref());
        // Results come back flat with shape (nq, k_eff); k_eff can be smaller
        // than k when the index (or allowlist) holds fewer vectors.
        let nq = req.vectors.len();
        let k_eff = if nq == 0 { 0 } else { ids.len() / nq };
        let scores_2d: Vec<&[f32]> = scores.chunks(k_eff.max(1)).take(nq).collect();
        let ids_2d: Vec<&[u64]> = ids.chunks(k_eff.max(1)).take(nq).collect();
        Ok(serde_json::json!({"scores": scores_2d, "ids": ids_2d}))
    }

    fn remove(&mut self, req: RemoveReq) -> Result<serde_json::Value, String> {
        let removed = req.ids.iter().filter(|id| self.index.remove(**id)).count();
        Ok(serde_json::json!({"removed": removed}))
    }

    fn persist(&self) -> Result<serde_json::Value, String> {
        let tmp = self.path.with_extension("tvim.tmp");
        self.index.write(&tmp).map_err(|e| format!("write: {e}"))?;
        std::fs::rename(&tmp, &self.path).map_err(|e| format!("rename: {e}"))?;
        Ok(serde_json::json!({"persisted": true, "count": self.index.len()}))
    }

    fn info(&self) -> serde_json::Value {
        serde_json::json!({
            "dim": self.index.dim_opt(),
            "count": self.index.len(),
            "bit_width": self.index.bit_width(),
        })
    }
}

fn respond(stream: &mut TcpStream, status: &str, body: &str) {
    let _ = write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    let _ = stream.flush();
}

fn ok(stream: &mut TcpStream, v: serde_json::Value) {
    respond(stream, "200 OK", &v.to_string());
}

fn err(stream: &mut TcpStream, status: &str, msg: &str) {
    respond(
        stream,
        status,
        &serde_json::json!({ "error": msg }).to_string(),
    );
}

/// Read one HTTP request: (method, path, body).
fn read_request(stream: &mut TcpStream) -> Result<(String, String, Vec<u8>), String> {
    let mut reader = BufReader::new(stream.try_clone().map_err(|e| e.to_string())?);
    let mut line = String::new();
    reader.read_line(&mut line).map_err(|e| e.to_string())?;
    let mut parts = line.split_whitespace();
    let method = parts.next().ok_or("bad request line")?.to_string();
    let path = parts.next().ok_or("bad request line")?.to_string();
    let mut content_length = 0usize;
    loop {
        let mut h = String::new();
        reader.read_line(&mut h).map_err(|e| e.to_string())?;
        let h = h.trim_end();
        if h.is_empty() {
            break;
        }
        if let Some(v) = h
            .to_ascii_lowercase()
            .strip_prefix("content-length:")
            .map(str::trim)
        {
            content_length = v.parse().map_err(|_| "bad content-length")?;
        }
    }
    let mut body = vec![0u8; content_length];
    reader.read_exact(&mut body).map_err(|e| e.to_string())?;
    Ok((method, path, body))
}

fn main() {
    let args = parse_args();

    let index = if args.path.exists() {
        match IdMapIndex::load(&args.path) {
            Ok(ix) => {
                eprintln!(
                    "turbovec-server: loaded {} vectors (dim {:?}) from {}",
                    ix.len(),
                    ix.dim_opt(),
                    args.path.display()
                );
                ix
            }
            Err(e) => die(&format!("failed to load {}: {e}", args.path.display())),
        }
    } else {
        let ix = match args.dim {
            Some(d) => IdMapIndex::new(d, args.bit_width),
            None => IdMapIndex::new_lazy(args.bit_width),
        };
        ix.unwrap_or_else(|e| die(&format!("index construction: {e:?}")))
    };

    let listener = TcpListener::bind(("127.0.0.1", args.port))
        .unwrap_or_else(|e| die(&format!("bind 127.0.0.1:{}: {e}", args.port)));
    let port = listener.local_addr().map(|a| a.port()).unwrap_or(args.port);
    println!("PORT {port}");
    // The orchestrator parses the PORT line; make sure it isn't stuck in a
    // stdio buffer when stdout is a pipe.
    let _ = std::io::stdout().flush();

    let mut server = Server {
        index,
        path: args.path,
    };

    for stream in listener.incoming() {
        let mut stream = match stream {
            Ok(s) => s,
            Err(_) => continue,
        };
        let (method, path, body) = match read_request(&mut stream) {
            Ok(r) => r,
            Err(e) => {
                err(&mut stream, "400 Bad Request", &e);
                continue;
            }
        };
        match (method.as_str(), path.as_str()) {
            ("GET", "/health") => ok(&mut stream, serde_json::json!({"status": "ok"})),
            ("GET", "/info") => {
                let v = server.info();
                ok(&mut stream, v)
            }
            ("POST", "/upsert") => match serde_json::from_slice::<UpsertReq>(&body) {
                Ok(req) => match server.upsert(req) {
                    Ok(v) => ok(&mut stream, v),
                    Err(e) => err(&mut stream, "422 Unprocessable Entity", &e),
                },
                Err(e) => err(&mut stream, "400 Bad Request", &e.to_string()),
            },
            ("POST", "/query") => match serde_json::from_slice::<QueryReq>(&body) {
                Ok(req) => match server.query(req) {
                    Ok(v) => ok(&mut stream, v),
                    Err(e) => err(&mut stream, "422 Unprocessable Entity", &e),
                },
                Err(e) => err(&mut stream, "400 Bad Request", &e.to_string()),
            },
            ("POST", "/remove") => match serde_json::from_slice::<RemoveReq>(&body) {
                Ok(req) => match server.remove(req) {
                    Ok(v) => ok(&mut stream, v),
                    Err(e) => err(&mut stream, "422 Unprocessable Entity", &e),
                },
                Err(e) => err(&mut stream, "400 Bad Request", &e.to_string()),
            },
            ("POST", "/persist") => match server.persist() {
                Ok(v) => ok(&mut stream, v),
                Err(e) => err(&mut stream, "500 Internal Server Error", &e),
            },
            ("POST", "/shutdown") => {
                match server.persist() {
                    Ok(_) => ok(&mut stream, serde_json::json!({"shutdown": true})),
                    Err(e) => err(&mut stream, "500 Internal Server Error", &e),
                }
                exit(0);
            }
            _ => err(&mut stream, "404 Not Found", "no such endpoint"),
        }
    }
}

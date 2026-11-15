//! Regenerates `rust/touchy-pad/src/proto/touchy.rs` from the shared
//! schemas in `proto/` using `prost-build`.
//!
//! The generated file is **checked in** on purpose, so that a consumer of
//! the published `touchy-pad` crate needs neither `protoc` nor a build
//! script. That mirrors what the Python package already does: its
//! `_proto/*_pb2.py` modules are gitignored in the source tree but bundled
//! pre-generated into the wheel, so `pip install touchy-pad` needs no
//! protoc either.
//!
//! Run this (or `just rust-gen-proto`, or `just build-proto`, which bundles
//! it alongside the Python/C bindings) whenever `proto/*.proto` changes and
//! commit the result. CI re-runs it and fails if the checked-in file is
//! stale, so drift cannot go unnoticed.
//!
//! This is the only place in the Rust tree that needs a `protoc` binary:
//! `prost-build` looks for it on `PATH`, falling back to `$PROTOC`.

use std::path::PathBuf;

/// The schemas all share `package touchy;`, so prost emits one flat
/// `touchy.rs` — the same file the old `touchy-pad/build.rs` produced.
const PROTOS: [&str; 3] = ["touchy.proto", "widgets.proto", "preferences.proto"];

/// Repo root — `rust/proto-gen/` is two levels down.
fn repo_root() -> PathBuf {
	PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn run() -> std::io::Result<()> {
	// `canonicalize` collapses the `../..` so the printed path is readable.
	let repo = repo_root().canonicalize()?;
	let proto_dir = repo.join("proto");
	let src_dir = repo.join("rust/touchy-pad/src/proto");

	std::fs::create_dir_all(&src_dir)?;

	let mut config = prost_build::Config::new();
	// Strip .proto comments. Otherwise rustdoc tries to compile any fenced
	// code block (or stray identifier like `LvEvent.host_code`) in the
	// generated doc-comments as a doctest, which fails.
	config.disable_comments(["."]);
	config.out_dir(&src_dir);
	config.compile_protos(&PROTOS.iter().map(|p| proto_dir.join(p)).collect::<Vec<_>>(), &[proto_dir])?;

	println!("wrote {}", src_dir.join("touchy.rs").display());
	Ok(())
}

fn main() {
	if let Err(e) = run() {
		eprintln!("proto-gen failed: {e}");
		eprintln!("hint: prost-build needs a `protoc` binary on PATH, or the PROTOC env var set to one.");
		eprintln!("      Install it (e.g. `apt-get install protobuf-compiler`) or download a release from");
		eprintln!("      https://github.com/protocolbuffers/protobuf/releases");
		std::process::exit(1);
	}
}

//! Build script: generate Rust types from the shared protobuf schemas
//! in `proto/` using `prost-build`.
//!
//! The output lands in `$OUT_DIR/touchy.rs` and is `include!`-d from
//! `src/proto.rs`. Re-runs whenever any of the schemas change.

use std::path::PathBuf;

fn main() {
	let proto_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("proto");
	let protos = ["touchy.proto", "widgets.proto", "preferences.proto"];

	for p in &protos {
		println!("cargo:rerun-if-changed={}/{}", proto_dir.display(), p);
	}

	let mut config = prost_build::Config::new();
	// Strip .proto comments. Otherwise rustdoc tries to compile any
	// fenced code block (or stray identifier like `LvEvent.host_code`)
	// in the generated doc-comments as a doctest, which fails.
	config.disable_comments(["."]);
	// All three .proto files share `package touchy;` so prost emits one
	// flat `touchy.rs`.
	config
		.compile_protos(&protos.iter().map(|p| proto_dir.join(p)).collect::<Vec<_>>(), &[proto_dir])
		.expect("failed to compile proto files");
}

//! Generated protobuf types.
//!
//! All three schemas in `proto/` share the same `package touchy;`
//! declaration, so prost emits a single `touchy.rs` containing every
//! message and enum. We re-export it under [`touchy_pad::proto`] so
//! downstream code writes `use touchy_pad::proto::Screen;` instead of
//! reaching into generated module paths.
#![allow(missing_docs)]

include!(concat!(env!("OUT_DIR"), "/touchy.rs"));

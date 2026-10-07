fn main() {
    println!("cargo:rerun-if-changed=proto/auth.proto");
    println!("cargo:rerun-if-changed=proto/searcher.proto");
    println!("cargo:rerun-if-changed=proto/packet.proto");
    println!("cargo:rerun-if-changed=proto/bundle.proto");
    println!("cargo:rerun-if-changed=proto/shared.proto");
    tonic_build::configure()
        .build_server(false)
        .compile_protos(
            &["proto/auth.proto", "proto/searcher.proto"],
            &["proto"],
        )
        .expect("official flowrawtf/mev-protos compile");
}

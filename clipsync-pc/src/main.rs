mod config;
mod protocol;

use config::ClipSyncConfig;

fn main() -> anyhow::Result<()> {
    env_logger::init();
    let cfg = ClipSyncConfig::load("clipsync.toml")?;
    log::info!("ClipSync PC starting... port={}", cfg.connection.port);
    Ok(())
}

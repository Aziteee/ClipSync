use crate::protocol::ClipMessage;
use futures_util::{SinkExt, StreamExt};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use tokio_tungstenite::connect_async;
use tokio_tungstenite::tungstenite::Message;

pub type WebSocketStream =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

pub async fn connect_and_auth(uri: &str, secret: &str) -> anyhow::Result<WebSocketStream> {
    let _url: url::Url = uri.parse()?;
    let (mut ws, _response) = connect_async(uri).await?;

    let raw = ws
        .next()
        .await
        .ok_or_else(|| anyhow::anyhow!("connection closed before hello"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message, got binary"),
    };

    let challenge = match msg {
        ClipMessage::Hello { challenge } => challenge,
        ClipMessage::AuthFail => anyhow::bail!("server rejected previous auth"),
        other => anyhow::bail!("expected hello, got {:?}", other),
    };

    let mut mac = Hmac::<Sha256>::new_from_slice(secret.as_bytes())
        .map_err(|_| anyhow::anyhow!("invalid secret key"))?;
    mac.update(challenge.as_bytes());
    let response = hex::encode(mac.finalize().into_bytes());

    let auth_msg = ClipMessage::Auth { response };
    ws.send(Message::Text(auth_msg.to_json())).await?;

    let raw = ws
        .next()
        .await
        .ok_or_else(|| anyhow::anyhow!("connection closed before auth result"))??;
    let msg = match raw {
        Message::Text(t) => ClipMessage::from_json(&t)?,
        _ => anyhow::bail!("expected text message"),
    };

    match msg {
        ClipMessage::AuthOk => {
            log::info!("HMAC authentication successful");
            Ok(ws)
        }
        ClipMessage::AuthFail => anyhow::bail!("authentication failed"),
        other => anyhow::bail!("expected auth_ok/auth_fail, got {:?}", other),
    }
}

pub async fn send(ws: &mut WebSocketStream, msg: &ClipMessage) -> anyhow::Result<()> {
    ws.send(Message::Text(msg.to_json())).await?;
    Ok(())
}

pub async fn recv(ws: &mut WebSocketStream) -> anyhow::Result<ClipMessage> {
    loop {
        let raw = ws
            .next()
            .await
            .ok_or_else(|| anyhow::anyhow!("connection closed"))??;
        match raw {
            Message::Text(t) => {
                if let Ok(msg) = ClipMessage::from_json(&t) {
                    return Ok(msg);
                }
            }
            Message::Ping(data) => {
                let _ = ws.send(Message::Pong(data)).await;
            }
            Message::Close(_) => {
                anyhow::bail!("connection closed by peer");
            }
            _ => {}
        }
    }
}

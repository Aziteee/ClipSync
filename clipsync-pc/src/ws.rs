use crate::protocol::ClipMessage;
use futures_util::{SinkExt, StreamExt};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::future::Future;
use std::time::Duration;
use tokio_tungstenite::connect_async;
use tokio_tungstenite::tungstenite::Message;

pub type WebSocketStream =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

pub async fn connect_and_auth(
    uri: &str,
    secret: &str,
    handshake_timeout: Duration,
) -> anyhow::Result<WebSocketStream> {
    let _url: url::Url = uri.parse()?;
    let (mut ws, _response) =
        with_timeout(connect_async(uri), handshake_timeout, "connect").await??;

    let raw = with_timeout(ws.next(), handshake_timeout, "hello")
        .await?
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
    with_timeout(
        ws.send(Message::Text(auth_msg.to_json())),
        handshake_timeout,
        "auth send",
    )
    .await??;

    let raw = with_timeout(ws.next(), handshake_timeout, "auth result")
        .await?
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

async fn with_timeout<F, T>(future: F, timeout: Duration, label: &str) -> anyhow::Result<T>
where
    F: Future<Output = T>,
{
    tokio::time::timeout(timeout, future)
        .await
        .map_err(|_| anyhow::anyhow!("{label} timed out after {}ms", timeout.as_millis()))
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

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn with_timeout_returns_error_after_deadline() {
        let result = with_timeout(
            tokio::time::sleep(Duration::from_millis(50)),
            Duration::from_millis(1),
            "test",
        )
        .await;

        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("test timed out"));
    }
}

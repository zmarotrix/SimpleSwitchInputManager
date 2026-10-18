using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Ssim.Tray;

/// <summary>
/// Talks to the SSIM engine over \\.\pipe\SSIM (one JSON object per line).
/// Subscribes to live status and sends commands on the same connection;
/// reconnects on its own when the engine (re)starts.
/// </summary>
public sealed class EngineClient : IDisposable
{
    public event Action<JsonObject>? StatusReceived;
    public event Action<bool>? ConnectionChanged;

    public bool Connected { get; private set; }

    private readonly CancellationTokenSource _cts = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly Dictionary<int, TaskCompletionSource<JsonObject>> _pending = new();
    private NamedPipeClientStream? _pipe;
    private int _nextId;

    public void Start() => _ = Task.Run(() => RunAsync(_cts.Token));

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var pipe = new NamedPipeClientStream(".", "SSIM", PipeDirection.InOut, PipeOptions.Asynchronous);
                await pipe.ConnectAsync(2000, ct);
                _pipe = pipe;
                SetConnected(true);
                await WriteAsync(new JsonObject { ["cmd"] = "subscribe" }, ct);

                using var reader = new StreamReader(pipe, Encoding.UTF8);
                while (!ct.IsCancellationRequested)
                {
                    string? line = await reader.ReadLineAsync(ct);
                    if (line == null) break;  // engine went away
                    Dispatch(line);
                }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                return;
            }
            catch (Exception)
            {
                // Engine not running yet, or the pipe broke: retry below.
            }
            _pipe = null;
            SetConnected(false);
            FailPending();
            try { await Task.Delay(2000, ct); } catch (OperationCanceledException) { return; }
        }
    }

    private void Dispatch(string line)
    {
        JsonObject? msg;
        try { msg = JsonNode.Parse(line) as JsonObject; } catch (JsonException) { return; }
        if (msg == null) return;

        if (msg["id"] is JsonValue idValue && idValue.TryGetValue(out int id))
        {
            TaskCompletionSource<JsonObject>? tcs;
            lock (_pending) _pending.Remove(id, out tcs);
            tcs?.TrySetResult(msg);
        }
        if ((string?)msg["type"] == "status") StatusReceived?.Invoke(msg);
    }

    /// <summary>Sends a command and returns the engine's reply (or an error object).</summary>
    public async Task<JsonObject> SendAsync(JsonObject request)
    {
        if (_pipe == null) return new JsonObject { ["ok"] = false, ["error"] = "SSIM engine is not running" };
        int id = Interlocked.Increment(ref _nextId);
        request["id"] = id;
        var tcs = new TaskCompletionSource<JsonObject>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_pending) _pending[id] = tcs;
        try
        {
            await WriteAsync(request, _cts.Token);
        }
        catch (Exception e)
        {
            lock (_pending) _pending.Remove(id);
            return new JsonObject { ["ok"] = false, ["error"] = e.Message };
        }
        var done = await Task.WhenAny(tcs.Task, Task.Delay(5000));
        if (done != tcs.Task)
        {
            lock (_pending) _pending.Remove(id);
            return new JsonObject { ["ok"] = false, ["error"] = "no reply from the engine" };
        }
        return tcs.Task.Result;
    }

    public Task<JsonObject> SetControllerAsync(string serial, string field, JsonNode value) =>
        SendAsync(new JsonObject { ["cmd"] = "setController", ["serial"] = serial, [field] = value });

    public Task<JsonObject> SetGlobalAsync(string field, JsonNode value) =>
        SendAsync(new JsonObject { ["cmd"] = "setGlobal", [field] = value });

    public Task<JsonObject> IdentifyAsync(string serial) =>
        SendAsync(new JsonObject { ["cmd"] = "identify", ["serial"] = serial });

    private async Task WriteAsync(JsonObject msg, CancellationToken ct)
    {
        var pipe = _pipe ?? throw new IOException("not connected");
        byte[] data = Encoding.UTF8.GetBytes(msg.ToJsonString() + "\n");
        await _writeLock.WaitAsync(ct);
        try
        {
            await pipe.WriteAsync(data, ct);
            await pipe.FlushAsync(ct);
        }
        finally
        {
            _writeLock.Release();
        }
    }

    private void SetConnected(bool connected)
    {
        if (Connected == connected) return;
        Connected = connected;
        ConnectionChanged?.Invoke(connected);
    }

    private void FailPending()
    {
        lock (_pending)
        {
            foreach (var tcs in _pending.Values)
                tcs.TrySetResult(new JsonObject { ["ok"] = false, ["error"] = "engine disconnected" });
            _pending.Clear();
        }
    }

    public void Dispose()
    {
        _cts.Cancel();
        _pipe?.Dispose();
    }
}

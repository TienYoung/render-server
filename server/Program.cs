using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Serialization;

Console.WriteLine("正在启动 HTTP signaling server...");
var builder = WebApplication.CreateSlimBuilder(args);

builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.TypeInfoResolverChain.Insert(0, AppJsonSerializerContext.Default);
});

var app = builder.Build();
var sessions = new ConcurrentDictionary<string, SignalingSession>();

app.UseDefaultFiles();
app.UseStaticFiles();

var signaling = app.MapGroup("/api/signaling");

signaling.MapPost("/sessions", () =>
{
    var session = SignalingSession.Create();
    sessions[session.Id] = session;

    Log(session.Id, "server", "创建会话并发起 offer");
    session.Add("offer", "server", "v=0\r\no=server 1 1 IN IP4 127.0.0.1\r\ns=mock-offer\r\nt=0 0");

    // 这些只是用来演示信令交换的文本，不会交给 RTCPeerConnection。
    session.Add("ice", "server", "candidate:server-1 1 udp 2122260223 192.0.2.1 50000 typ host");
    session.Add("ice", "server", "candidate:server-2 1 udp 1686052607 198.51.100.1 3478 typ srflx");
    Log(session.Id, "server", "已生成 2 个模拟 ICE candidate");

    return Results.Ok(new SessionCreated(session.Id));
});

signaling.MapGet("/sessions/{id}/events", async (HttpContext context, string id, long? after) =>
{
    if (!sessions.TryGetValue(id, out var session))
    {
        context.Response.StatusCode = StatusCodes.Status404NotFound;
        return;
    }

    context.Response.Headers.CacheControl = "no-cache";
    context.Response.Headers.Append("X-Accel-Buffering", "no");
    context.Response.ContentType = "text/event-stream";

    var lastSequence = after ?? 0;
    if (after is null && long.TryParse(context.Request.Headers["Last-Event-ID"], out var lastEventId))
    {
        lastSequence = lastEventId;
    }
    await context.Response.WriteAsync("retry: 1000\n\n", context.RequestAborted);
    await context.Response.Body.FlushAsync(context.RequestAborted);

    try
    {
        while (!context.RequestAborted.IsCancellationRequested)
        {
            var events = await session.WaitForEventsAfterAsync(lastSequence, context.RequestAborted);
            foreach (var signalEvent in events)
            {
                var json = JsonSerializer.Serialize(signalEvent, AppJsonSerializerContext.Default.SignalEvent);
                await context.Response.WriteAsync($"id: {signalEvent.Sequence}\nevent: signal\ndata: {json}\n\n", context.RequestAborted);
                lastSequence = signalEvent.Sequence;
            }

            await context.Response.Body.FlushAsync(context.RequestAborted);
        }
    }
    catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
    {
        // 浏览器关闭 EventSource 时正常结束连接。
    }
});

signaling.MapPost("/sessions/{id}/answer", (string id, AnswerRequest request) =>
{
    if (!sessions.TryGetValue(id, out var session))
    {
        return Results.NotFound();
    }

    session.Add("answer", "client", request.Sdp);
    Log(id, "client", $"收到 answer: {request.Sdp.Replace("\r", "").Replace("\n", " | ")}");
    return Results.NoContent();
});

signaling.MapPost("/sessions/{id}/ice", (string id, IceCandidateRequest request) =>
{
    if (!sessions.TryGetValue(id, out var session))
    {
        return Results.NotFound();
    }

    session.Add("ice", "client", request.Candidate);
    Log(id, "client", $"收到 ICE candidate: {request.Candidate}");
    return Results.NoContent();
});

app.Run();

static void Log(string sessionId, string sender, string message) =>
    Console.WriteLine("[{0:HH:mm:ss}] [{1}] [{2}] {3}", DateTimeOffset.Now, sessionId, sender, message);

public sealed class SignalingSession
{
    private readonly object gate = new();
    private readonly List<SignalEvent> events = [];
    private TaskCompletionSource eventsChanged = NewEventsChangedSource();
    private long sequence;

    private SignalingSession(string id) => Id = id;

    public string Id { get; }

    public static SignalingSession Create() => new(Guid.NewGuid().ToString("N")[..8]);

    public void Add(string type, string sender, string payload)
    {
        TaskCompletionSource changed;
        lock (gate)
        {
            events.Add(new SignalEvent(++sequence, type, sender, payload, DateTimeOffset.UtcNow));
            changed = eventsChanged;
            eventsChanged = NewEventsChangedSource();
        }

        changed.TrySetResult();
    }

    public async Task<SignalEvent[]> WaitForEventsAfterAsync(long after, CancellationToken cancellationToken)
    {
        while (true)
        {
            Task changed;
            lock (gate)
            {
                var available = events.Where(item => item.Sequence > after).ToArray();
                if (available.Length > 0)
                {
                    return available;
                }

                changed = eventsChanged.Task;
            }

            await changed.WaitAsync(cancellationToken);
        }
    }

    private static TaskCompletionSource NewEventsChangedSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

public sealed record SessionCreated(string SessionId);
public sealed record AnswerRequest(string Sdp);
public sealed record IceCandidateRequest(string Candidate);
public sealed record SignalEvent(long Sequence, string Type, string Sender, string Payload, DateTimeOffset CreatedAt);

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(SessionCreated))]
[JsonSerializable(typeof(AnswerRequest))]
[JsonSerializable(typeof(IceCandidateRequest))]
[JsonSerializable(typeof(SignalEvent[]))]
internal partial class AppJsonSerializerContext : JsonSerializerContext;

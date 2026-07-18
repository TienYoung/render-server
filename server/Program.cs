using System.Text.Json;
using System.Text.Json.Serialization;

Console.WriteLine("Starting fixed-role HTTP/SSE signaling server...");
var builder = WebApplication.CreateSlimBuilder(args);

builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.TypeInfoResolverChain.Insert(0, AppJsonSerializerContext.Default);
});

var app = builder.Build();
var hub = new SignalingHub();

app.UseDefaultFiles();
app.UseStaticFiles();

var signaling = app.MapGroup("/api/signaling");

// Render sends offer and ICE messages to the viewer over HTTP.
signaling.MapPost("/render/offer", (SdpRequest request) =>
{
    hub.BeginOffer(request.Sdp);
    Log("render", "sent offer", request.Sdp);
    return Results.NoContent();
});

signaling.MapPost("/render/ice", (IceCandidateRequest request) =>
{
    hub.SendToViewer("ice", "render", request.Candidate);
    Log("render", "sent ICE", request.Candidate);
    return Results.NoContent();
});

// Render receives only viewer answer and ICE messages from this SSE stream.
signaling.MapGet("/render/events", (HttpContext context, long? after) =>
    StreamEvents(context, after, hub.WaitForRenderEventsAfterAsync));

// Viewer sends answer and ICE messages to Render over HTTP.
signaling.MapPost("/viewer/answer", (SdpRequest request) =>
{
    hub.SendToRender("answer", "viewer", request.Sdp);
    Log("viewer", "sent answer", request.Sdp);
    return Results.NoContent();
});

signaling.MapPost("/viewer/ice", (IceCandidateRequest request) =>
{
    hub.SendToRender("ice", "viewer", request.Candidate);
    Log("viewer", "sent ICE", request.Candidate);
    return Results.NoContent();
});

// Viewer receives only Render offer and ICE messages from this SSE stream.
signaling.MapGet("/viewer/events", (HttpContext context, long? after) =>
    StreamEvents(context, after, hub.WaitForViewerEventsAfterAsync));

app.Run();

static async Task StreamEvents(
    HttpContext context,
    long? after,
    Func<long, CancellationToken, Task<SignalEvent[]>> waitForEvents)
{
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
            var events = await waitForEvents(lastSequence, context.RequestAborted);
            foreach (var signalEvent in events)
            {
                var json = JsonSerializer.Serialize(signalEvent, AppJsonSerializerContext.Default.SignalEvent);
                await context.Response.WriteAsync(
                    $"id: {signalEvent.Sequence}\nevent: signal\ndata: {json}\n\n",
                    context.RequestAborted);
                lastSequence = signalEvent.Sequence;
            }

            await context.Response.Body.FlushAsync(context.RequestAborted);
        }
    }
    catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
    {
        // End normally when the SSE client disconnects.
    }
}

static void Log(string sender, string action, string payload) =>
    Console.WriteLine(
        "[{0:HH:mm:ss}] [{1}] {2}: {3}",
        DateTimeOffset.Now,
        sender,
        action,
        payload.Replace("\r", "").Replace("\n", " | "));

public sealed class SignalingHub
{
    private readonly object gate = new();
    private readonly List<SignalEvent> viewerEvents = [];
    private readonly List<SignalEvent> renderEvents = [];
    private TaskCompletionSource viewerEventsChanged = NewEventsChangedSource();
    private TaskCompletionSource renderEventsChanged = NewEventsChangedSource();
    private long sequence;

    public void BeginOffer(string sdp)
    {
        TaskCompletionSource viewerChanged;
        TaskCompletionSource renderChanged;

        lock (gate)
        {
            // Only one Render-to-Viewer pair is supported. A new offer starts a new negotiation.
            viewerEvents.Clear();
            renderEvents.Clear();
            viewerEvents.Add(new SignalEvent(++sequence, "offer", "render", sdp, DateTimeOffset.UtcNow));

            viewerChanged = viewerEventsChanged;
            renderChanged = renderEventsChanged;
            viewerEventsChanged = NewEventsChangedSource();
            renderEventsChanged = NewEventsChangedSource();
        }

        viewerChanged.TrySetResult();
        renderChanged.TrySetResult();
    }

    public void SendToViewer(string type, string sender, string payload) =>
        Add(viewerEvents, ref viewerEventsChanged, type, sender, payload);

    public void SendToRender(string type, string sender, string payload) =>
        Add(renderEvents, ref renderEventsChanged, type, sender, payload);

    public Task<SignalEvent[]> WaitForViewerEventsAfterAsync(long after, CancellationToken cancellationToken) =>
        WaitForEventsAfterAsync(viewerEvents, () => viewerEventsChanged.Task, after, cancellationToken);

    public Task<SignalEvent[]> WaitForRenderEventsAfterAsync(long after, CancellationToken cancellationToken) =>
        WaitForEventsAfterAsync(renderEvents, () => renderEventsChanged.Task, after, cancellationToken);

    private void Add(
        List<SignalEvent> destination,
        ref TaskCompletionSource eventsChanged,
        string type,
        string sender,
        string payload)
    {
        TaskCompletionSource changed;
        lock (gate)
        {
            destination.Add(new SignalEvent(++sequence, type, sender, payload, DateTimeOffset.UtcNow));
            changed = eventsChanged;
            eventsChanged = NewEventsChangedSource();
        }

        changed.TrySetResult();
    }

    private async Task<SignalEvent[]> WaitForEventsAfterAsync(
        List<SignalEvent> source,
        Func<Task> getChangedTask,
        long after,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            Task changed;
            lock (gate)
            {
                var available = source.Where(item => item.Sequence > after).ToArray();
                if (available.Length > 0)
                {
                    return available;
                }

                changed = getChangedTask();
            }

            await changed.WaitAsync(cancellationToken);
        }
    }

    private static TaskCompletionSource NewEventsChangedSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

public sealed record SdpRequest(string Sdp);
public sealed record IceCandidateRequest(string Candidate);
public sealed record SignalEvent(long Sequence, string Type, string Sender, string Payload, DateTimeOffset CreatedAt);

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(SdpRequest))]
[JsonSerializable(typeof(IceCandidateRequest))]
[JsonSerializable(typeof(SignalEvent))]
internal partial class AppJsonSerializerContext : JsonSerializerContext;

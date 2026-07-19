using System.Net.ServerSentEvents;
using System.Runtime.CompilerServices;
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

// Render sends offer and ICE messages to the Viewer over HTTP.
signaling.MapPost("/render/offer", (SdpRequest request) =>
{
    hub.BeginOffer(request.Sdp);
    LogDescription("render", "sent offer", request.Sdp);
    return Results.NoContent();
});

signaling.MapPost("/render/ice", (IceCandidateMessage request) =>
{
    hub.SendIceToViewer(request);
    LogIce("render", request);
    return Results.NoContent();
});

// Render receives only Viewer answer and ICE messages from this SSE stream.
signaling.MapGet("/render/events", (HttpContext context, long? after) =>
    CreateEventStream(context, after, hub.WaitForRenderEventsAfterAsync));

// Viewer sends answer and ICE messages to Render over HTTP.
signaling.MapPost("/viewer/answer", (SdpRequest request) =>
{
    hub.SendAnswerToRender(request.Sdp);
    LogDescription("viewer", "sent answer", request.Sdp);
    return Results.NoContent();
});

signaling.MapPost("/viewer/ice", (IceCandidateMessage request) =>
{
    hub.SendIceToRender(request);
    LogIce("viewer", request);
    return Results.NoContent();
});

// Viewer receives only Render offer and ICE messages from this SSE stream.
signaling.MapGet("/viewer/events", (HttpContext context, long? after) =>
    CreateEventStream(context, after, hub.WaitForViewerEventsAfterAsync));

app.Run();

static IResult CreateEventStream(
    HttpContext context,
    long? after,
    Func<long, CancellationToken, Task<SignalEvent[]>> waitForEvents)
{
    context.Response.Headers.Append("X-Accel-Buffering", "no");

    var lastSequence = after ?? 0;
    if (after is null && long.TryParse(context.Request.Headers["Last-Event-ID"], out var lastEventId))
    {
        lastSequence = lastEventId;
    }

    return TypedResults.ServerSentEvents(
        ReadEvents(lastSequence, waitForEvents, context.RequestAborted));
}

static async IAsyncEnumerable<SseItem<SignalEvent>> ReadEvents(
    long lastSequence,
    Func<long, CancellationToken, Task<SignalEvent[]>> waitForEvents,
    [EnumeratorCancellation] CancellationToken cancellationToken)
{
    while (!cancellationToken.IsCancellationRequested)
    {
        SignalEvent[] events;
        try
        {
            events = await waitForEvents(lastSequence, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            yield break;
        }

        foreach (var signalEvent in events)
        {
            yield return new SseItem<SignalEvent>(signalEvent, "signal")
            {
                EventId = signalEvent.Sequence.ToString(),
                ReconnectionInterval = TimeSpan.FromSeconds(1)
            };
            lastSequence = signalEvent.Sequence;
        }
    }
}

static void LogDescription(string sender, string action, string sdp) =>
    Console.WriteLine(
        "[{0:HH:mm:ss}] [{1}] {2}: {3}",
        DateTimeOffset.Now,
        sender,
        action,
        sdp.Replace("\r", "").Replace("\n", " | "));

static void LogIce(string sender, IceCandidateMessage ice) =>
    Console.WriteLine(
        "[{0:HH:mm:ss}] [{1}] sent ICE: candidate={2}, sdpMid={3}, sdpMLineIndex={4}, usernameFragment={5}",
        DateTimeOffset.Now,
        sender,
        ice.Candidate,
        ice.SdpMid,
        ice.SdpMLineIndex,
        ice.UsernameFragment);

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
            viewerEvents.Add(NewEvent("offer", "render", sdp, null));

            viewerChanged = viewerEventsChanged;
            renderChanged = renderEventsChanged;
            viewerEventsChanged = NewEventsChangedSource();
            renderEventsChanged = NewEventsChangedSource();
        }

        viewerChanged.TrySetResult();
        renderChanged.TrySetResult();
    }

    public void SendIceToViewer(IceCandidateMessage ice) =>
        Add(viewerEvents, ref viewerEventsChanged, "ice", "render", null, ice);

    public void SendAnswerToRender(string sdp) =>
        Add(renderEvents, ref renderEventsChanged, "answer", "viewer", sdp, null);

    public void SendIceToRender(IceCandidateMessage ice) =>
        Add(renderEvents, ref renderEventsChanged, "ice", "viewer", null, ice);

    public Task<SignalEvent[]> WaitForViewerEventsAfterAsync(long after, CancellationToken cancellationToken) =>
        WaitForEventsAfterAsync(viewerEvents, () => viewerEventsChanged.Task, after, cancellationToken);

    public Task<SignalEvent[]> WaitForRenderEventsAfterAsync(long after, CancellationToken cancellationToken) =>
        WaitForEventsAfterAsync(renderEvents, () => renderEventsChanged.Task, after, cancellationToken);

    private void Add(
        List<SignalEvent> destination,
        ref TaskCompletionSource eventsChanged,
        string type,
        string sender,
        string? sdp,
        IceCandidateMessage? ice)
    {
        TaskCompletionSource changed;
        lock (gate)
        {
            destination.Add(NewEvent(type, sender, sdp, ice));
            changed = eventsChanged;
            eventsChanged = NewEventsChangedSource();
        }

        changed.TrySetResult();
    }

    private SignalEvent NewEvent(string type, string sender, string? sdp, IceCandidateMessage? ice) =>
        new(++sequence, type, sender, sdp, ice, DateTimeOffset.UtcNow);

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

public sealed record IceCandidateMessage(
    string Candidate,
    string? SdpMid,
    int? SdpMLineIndex,
    string? UsernameFragment);

public sealed record SignalEvent(
    long Sequence,
    string Type,
    string Sender,
    string? Sdp,
    IceCandidateMessage? Ice,
    DateTimeOffset CreatedAt);

[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(SdpRequest))]
[JsonSerializable(typeof(IceCandidateMessage))]
[JsonSerializable(typeof(SignalEvent))]
internal partial class AppJsonSerializerContext : JsonSerializerContext;

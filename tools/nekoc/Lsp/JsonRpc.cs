using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Nekotia.Compiler.Lsp;

/// <summary>LSP base protocol over stdio: "Content-Length: n\r\n\r\n" + JSON-RPC 2.0.</summary>
public sealed class JsonRpc(Stream input, Stream output)
{
    readonly object _writeLock = new();

    public JsonObject? Read()
    {
        int length = -1;
        while (true)
        {
            var line = ReadHeaderLine();
            if (line is null)
                return null;
            if (line.Length == 0)
                break;
            if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                length = int.Parse(line["Content-Length:".Length..].Trim());
        }
        if (length < 0)
            return null;
        var buffer = new byte[length];
        int read = 0;
        while (read < length)
        {
            int n = input.Read(buffer, read, length - read);
            if (n <= 0)
                return null;
            read += n;
        }
        return JsonNode.Parse(buffer) as JsonObject;
    }

    string? ReadHeaderLine()
    {
        var sb = new StringBuilder();
        while (true)
        {
            int b = input.ReadByte();
            if (b < 0)
                return null;
            if (b == '\n')
                return sb.ToString().TrimEnd('\r');
            sb.Append((char)b);
        }
    }

    public void Send(JsonObject message)
    {
        message["jsonrpc"] = "2.0";
        var body = Encoding.UTF8.GetBytes(message.ToJsonString(Options));
        var header = Encoding.ASCII.GetBytes($"Content-Length: {body.Length}\r\n\r\n");
        lock (_writeLock)
        {
            output.Write(header);
            output.Write(body);
            output.Flush();
        }
    }

    public void Respond(JsonNode? id, JsonNode? result) => Send(new JsonObject { ["id"] = id?.DeepClone(), ["result"] = result });

    public void RespondError(JsonNode? id, int code, string message) =>
        Send(new JsonObject { ["id"] = id?.DeepClone(), ["error"] = new JsonObject { ["code"] = code, ["message"] = message } });

    public void Notify(string method, JsonNode? @params) => Send(new JsonObject { ["method"] = method, ["params"] = @params });

    static readonly JsonSerializerOptions Options = new() { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
}

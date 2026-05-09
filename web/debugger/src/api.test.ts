import { describe, expect, it, vi } from "vitest";
import { candidatePorts, discoverServers, entityLabel, ENTITIES_QUERY, type Fetcher, GraphQLClient } from "./api";

function jsonResponse(body: unknown, ok = true, status = 200): Response {
  return {
    ok,
    status,
    json: async () => body
  } as Response;
}

describe("candidatePorts", () => {
  it("returns the base port and next three ports", () => {
    expect(candidatePorts(9000)).toEqual([9000, 9001, 9002, 9003]);
  });
});

describe("GraphQLClient", () => {
  it("formats GraphQL requests and returns data", async () => {
    const fetchImpl = vi.fn(async () => jsonResponse({ data: { ok: true } })) as unknown as Fetcher;
    const client = new GraphQLClient("http://127.0.0.1:8080/graphql", fetchImpl);

    await expect(client.request("{ entities { id } }", { id: "1" })).resolves.toEqual({ ok: true });

    expect(fetchImpl).toHaveBeenCalledWith(
      "http://127.0.0.1:8080/graphql",
      expect.objectContaining({
        method: "POST",
        headers: { "Content-Type": "text/plain" },
        body: JSON.stringify({ query: "{ entities { id } }", variables: { id: "1" } })
      })
    );
  });

  it("surfaces GraphQL errors", async () => {
    const fetchImpl = vi.fn(async () =>
      jsonResponse({ errors: [{ message: "bad component" }] })
    ) as unknown as Fetcher;
    const client = new GraphQLClient("http://127.0.0.1:8080/graphql", fetchImpl);

    await expect(client.request(ENTITIES_QUERY)).rejects.toThrow("bad component");
  });
});

describe("entityLabel", () => {
  it("prefers debug display names", () => {
    expect(entityLabel({ id: "1", index: 1, version: 1, kind: "User", displayName: "ball 1" })).toBe("ball 1");
  });
});

describe("discoverServers", () => {
  it("reports partial discovery success", async () => {
    const fetchImpl = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (url.includes(":9001/")) {
        const body = typeof init?.body === "string" ? init.body : "";
        if (body.includes("serverName")) {
          return jsonResponse({ data: { serverName: "client 1" } });
        }
        if (body.includes("registeredTags")) {
          return jsonResponse({ data: { registeredTags: [{ component: "2", name: "Glow" }] } });
        }
        if (body.includes("singletons")) {
          return jsonResponse({ data: { singletons: [] } });
        }
        if (body.includes("jobs")) {
          return jsonResponse({ data: { jobs: [] } });
        }
        return jsonResponse({ data: { entities: [{ id: "1", index: 1, version: 1, kind: "User" }] } });
      }
      throw new Error("offline");
    }) as unknown as Fetcher;

    const servers = await discoverServers(9000, fetchImpl);

    expect(servers.map((server) => server.port)).toEqual([9000, 9001, 9002, 9003]);
    expect(servers.filter((server) => server.ok).map((server) => server.port)).toEqual([9001]);
    expect(servers[1].snapshot?.name).toBe("client 1");
    expect(servers[1].snapshot?.entities).toHaveLength(1);
    expect(servers[1].snapshot?.singletons).toHaveLength(0);
  });
});

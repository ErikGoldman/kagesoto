import { describe, expect, it } from "vitest";
import { reconcileSelection, type DebuggerState } from "./state";
import type { DiscoveredServer } from "./types";

const servers: DiscoveredServer[] = [
  {
    port: 8080,
    url: "http://127.0.0.1:8080/graphql",
    ok: true,
    latencyMs: 3,
    snapshot: {
      name: "server",
      entities: [
        { id: "10", index: 10, version: 1, kind: "User" },
        { id: "11", index: 11, version: 1, kind: "System" }
      ],
      singletons: [
        {
          entity: { id: "12", index: 12, version: 1, kind: "System" },
          component: {
            component: "30",
            name: "GameTime",
            tag: false,
            singleton: true,
            dirty: false,
            debugValue: "GameTime{tick=0}",
            fields: []
          }
        }
      ],
      jobs: [{ id: "20", name: "Move balls", order: 1, structural: false, singleThread: true, maxThreads: 1, minEntitiesPerThread: 1, reads: [], writes: [] }],
      registeredTags: []
    }
  }
];

describe("reconcileSelection", () => {
  it("selects the first live server, entity, and job by default", () => {
    const next = reconcileSelection({ servers: [] }, servers);

    expect(next.selectedPort).toBe(8080);
    expect(next.entityPanelTab).toBe("entities");
    expect(next.selectedEntityId).toBe("10");
    expect(next.selectedSingletonComponentId).toBe("30");
    expect(next.selectedJobId).toBe("20");
  });

  it("preserves entity selections when still present", () => {
    const previous: DebuggerState = {
      servers,
      selectedPort: 8080,
      entityPanelTab: "entities",
      selectedEntityId: "11",
      selectedSingletonComponentId: "30",
      selectedJobId: "20",
      entityDetail: { id: "11", index: 11, version: 1, kind: "System", components: [], matchingJobs: [] },
      jobDetail: {
        id: "20",
        name: "Move balls",
        order: 1,
        structural: false,
        singleThread: true,
        maxThreads: 1,
        minEntitiesPerThread: 1,
        reads: [],
        writes: [],
        matchingEntities: []
      }
    };

    const next = reconcileSelection(previous, servers);

    expect(next.selectedEntityId).toBe("11");
    expect(next.selectedSingletonComponentId).toBe("30");
    expect(next.entityPanelTab).toBe("entities");
    expect(next.entityDetail).toBe(previous.entityDetail);
    expect(next.jobDetail).toBe(previous.jobDetail);
  });

  it("preserves the singleton tab and selection when still present", () => {
    const next = reconcileSelection(
      {
        servers,
        selectedPort: 8080,
        entityPanelTab: "singletons",
        selectedSingletonComponentId: "30"
      },
      servers
    );

    expect(next.entityPanelTab).toBe("singletons");
    expect(next.selectedSingletonComponentId).toBe("30");
  });
});

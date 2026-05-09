import type { DiscoveredServer, EntityDetail, JobDetail } from "./types";

export type EntityPanelTab = "entities" | "singletons";

export interface DebuggerState {
  servers: DiscoveredServer[];
  selectedPort?: number;
  entityPanelTab?: EntityPanelTab;
  selectedEntityId?: string;
  selectedSingletonComponentId?: string;
  selectedJobId?: string;
  entityDetail?: EntityDetail | null;
  jobDetail?: JobDetail | null;
}

export function reconcileSelection(previous: DebuggerState, servers: DiscoveredServer[]): DebuggerState {
  const aliveServers = servers.filter((server) => server.ok);
  const selectedPort =
    previous.selectedPort && aliveServers.some((server) => server.port === previous.selectedPort)
      ? previous.selectedPort
      : aliveServers[0]?.port;
  const selectedServer = aliveServers.find((server) => server.port === selectedPort);
  const entities = selectedServer?.snapshot?.entities ?? [];
  const singletons = selectedServer?.snapshot?.singletons ?? [];
  const jobs = selectedServer?.snapshot?.jobs ?? [];
  const selectedSingletonComponentId =
    previous.selectedSingletonComponentId &&
    singletons.some((singleton) => singleton.component.component === previous.selectedSingletonComponentId)
      ? previous.selectedSingletonComponentId
      : singletons[0]?.component.component;
  const entityPanelTab: EntityPanelTab =
    previous.entityPanelTab === "singletons" && selectedSingletonComponentId
      ? "singletons"
      : previous.entityPanelTab === "entities" && entities.length > 0
        ? "entities"
        : entities.length > 0
          ? "entities"
          : selectedSingletonComponentId
            ? "singletons"
            : previous.entityPanelTab ?? "entities";
  const selectedEntityId =
    previous.selectedEntityId && entities.some((entity) => entity.id === previous.selectedEntityId)
      ? previous.selectedEntityId
      : entities[0]?.id;
  const selectedJobId =
    previous.selectedJobId && jobs.some((job) => job.id === previous.selectedJobId)
      ? previous.selectedJobId
      : jobs[0]?.id;

  return {
    ...previous,
    servers,
    selectedPort,
    entityPanelTab,
    selectedEntityId,
    selectedSingletonComponentId,
    selectedJobId,
    entityDetail:
      previous.selectedEntityId === selectedEntityId && previous.selectedPort === selectedPort
        ? previous.entityDetail
        : undefined,
    jobDetail:
      previous.selectedJobId === selectedJobId && previous.selectedPort === selectedPort ? previous.jobDetail : undefined
  };
}

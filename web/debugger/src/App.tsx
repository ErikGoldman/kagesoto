import { useCallback, useEffect, useMemo, useState } from "react";
import type React from "react";
import {
  Activity,
  Briefcase,
  CheckCircle2,
  CircleAlert,
  Database,
  PlugZap,
  Plus,
  RefreshCw,
  Search,
  Server,
  SlidersHorizontal,
  Tag,
  Trash2
} from "lucide-react";
import {
  addTag,
  componentRefId,
  componentRefIsSingleton,
  componentRefName,
  discoverServers,
  entityLabel,
  fetchEntity,
  fetchJob,
  GraphQLClient,
  jobTitle,
  removeComponent,
  serverUrl,
  setComponent
} from "./api";
import { reconcileSelection, type DebuggerState } from "./state";
import type {
  ComponentFieldValue,
  ComponentInstance,
  ComponentRef,
  EntitySummary,
  JobSummary,
  RegisteredTag,
  SingletonInstance
} from "./types";

const AUTO_REFRESH_MS = 1500;
const DEFAULT_BASE_PORT = 8080;
const BASE_PORT_STORAGE_KEY = "ashiato-debugger.basePort";
const EMPTY_COMPONENT_FIELDS: ComponentFieldValue[] = [];

export function App() {
  const [basePort, setBasePort] = useState(readStoredBasePort);
  const [state, setState] = useState<DebuggerState>({ servers: [] });
  const [entityQuery, setEntityQuery] = useState("");
  const [jobQuery, setJobQuery] = useState("");
  const [loading, setLoading] = useState(false);
  const [autoRefresh, setAutoRefresh] = useState(false);
  const [error, setError] = useState<string>();

  const selectedServer = useMemo(
    () => state.servers.find((server) => server.port === state.selectedPort),
    [state.selectedPort, state.servers]
  );
  const client = useMemo(
    () => (selectedServer?.ok ? new GraphQLClient(serverUrl(selectedServer.port)) : undefined),
    [selectedServer]
  );

  const refresh = useCallback(async () => {
    setLoading(true);
    setError(undefined);
    try {
      const servers = await discoverServers(basePort);
      setState((previous) => reconcileSelection(previous, servers));
    } catch (refreshError) {
      setError(refreshError instanceof Error ? refreshError.message : String(refreshError));
    } finally {
      setLoading(false);
    }
  }, [basePort]);

  const loadDetails = useCallback(async () => {
    if (!client) {
      return;
    }
    try {
      const [entityDetail, jobDetail] = await Promise.all([
        state.selectedEntityId ? fetchEntity(client, state.selectedEntityId) : Promise.resolve(null),
        state.selectedJobId ? fetchJob(client, state.selectedJobId) : Promise.resolve(null)
      ]);
      setState((previous) => ({ ...previous, entityDetail, jobDetail }));
    } catch (detailError) {
      setError(detailError instanceof Error ? detailError.message : String(detailError));
    }
  }, [client, state.selectedEntityId, state.selectedJobId]);

  useEffect(() => {
    void refresh();
  }, []);

  useEffect(() => {
    try {
      window.localStorage.setItem(BASE_PORT_STORAGE_KEY, String(basePort));
    } catch {
      // Ignore unavailable localStorage, for example in private or restricted browser contexts.
    }
  }, [basePort]);

  useEffect(() => {
    void loadDetails();
  }, [loadDetails]);

  useEffect(() => {
    if (!autoRefresh) {
      return;
    }
    const timer = window.setInterval(() => {
      void refresh();
    }, AUTO_REFRESH_MS);
    return () => window.clearInterval(timer);
  }, [autoRefresh, refresh]);

  const aliveServers = state.servers.filter((server) => server.ok);
  const visibleScanErrors = state.servers.filter((server) => !server.ok && server.port === basePort);
  const entities = selectedServer?.snapshot?.entities ?? [];
  const singletons = selectedServer?.snapshot?.singletons ?? [];
  const jobs = selectedServer?.snapshot?.jobs ?? [];
  const entityPanelTab = state.entityPanelTab ?? "entities";
  const filteredEntities = entities.filter((entity) => matchesEntity(entity, entityQuery));
  const filteredSingletons = singletons.filter((singleton) => matchesSingleton(singleton, entityQuery));
  const selectedSingleton =
    singletons.find((singleton) => singleton.component.component === state.selectedSingletonComponentId) ?? null;
  const filteredJobs = jobs.filter((job) => matchesJob(job, jobQuery));
  const registeredTags = selectedServer?.snapshot?.registeredTags ?? [];

  return (
    <div className="app-shell">
      <header className="topbar">
        <div className="title">
          <Database size={22} />
          <div>
            <h1>Ashiato Debugger</h1>
            <span>{aliveServers.length} server{aliveServers.length === 1 ? "" : "s"} connected</span>
          </div>
        </div>
        <div className="connection-controls">
          <label>
            Base port
            <input
              type="number"
              min={1}
              max={65532}
              value={basePort}
              onChange={(event) => {
                const nextPort = Number(event.target.value);
                setBasePort(validBasePort(nextPort) ? nextPort : DEFAULT_BASE_PORT);
              }}
            />
          </label>
          <button className="icon-button primary" onClick={() => void refresh()} disabled={loading}>
            <RefreshCw size={16} className={loading ? "spin" : ""} />
            Refresh
          </button>
          <label className="toggle">
            <input type="checkbox" checked={autoRefresh} onChange={(event) => setAutoRefresh(event.target.checked)} />
            Auto
          </label>
        </div>
      </header>

      <section className="server-strip">
        {state.servers.length === 0 ? (
          <EmptyInline icon={<PlugZap size={18} />} text="No scan has completed yet." />
        ) : (
          aliveServers.map((server) => (
            <button
              key={server.port}
              className={`server-chip ${server.port === state.selectedPort ? "selected" : ""} ok`}
              onClick={() =>
                setState((previous) =>
                  reconcileSelection({ ...previous, selectedPort: server.port }, previous.servers)
                )
              }
            >
              <CheckCircle2 size={16} />
              <span>{server.snapshot?.name || `:${server.port}`}</span>
              {server.snapshot && (
                <small>
                  :{server.port} - {server.snapshot.entities.length} entities, {server.snapshot.jobs.length} jobs
                </small>
              )}
            </button>
          ))
        )}
      </section>

      {visibleScanErrors.length > 0 && (
        <section className="scan-errors">
          {visibleScanErrors.map((server) => (
            <span key={server.port}>
              :{server.port} {server.error || "unreachable"}
            </span>
          ))}
        </section>
      )}

      {error && (
        <div className="error-banner">
          <CircleAlert size={16} />
          {error}
        </div>
      )}

      {!selectedServer ? (
        <main className="empty-stage">
          <Server size={36} />
          <h2>No debug server found</h2>
          <p>Scan checks localhost ports {basePort} through {basePort + 3}.</p>
        </main>
      ) : (
        <main className="debug-grid">
          <Panel
            title="Entities"
            icon={<Database size={17} />}
            count={entityPanelTab === "singletons" ? singletons.length : entities.length}
          >
            <div className="tab-strip">
              <button
                className={entityPanelTab === "entities" ? "selected" : ""}
                onClick={() => setState((previous) => ({ ...previous, entityPanelTab: "entities" }))}
              >
                Entities
              </button>
              <button
                className={entityPanelTab === "singletons" ? "selected" : ""}
                onClick={() => setState((previous) => ({ ...previous, entityPanelTab: "singletons" }))}
              >
                Singletons
              </button>
            </div>
            <SearchBox
              value={entityQuery}
              onChange={setEntityQuery}
              placeholder={
                entityPanelTab === "singletons" ? "Filter by component, value, id..." : "Filter by id, kind, index..."
              }
            />
            {entityPanelTab === "singletons" ? (
              <div className="list">
                {filteredSingletons.map((singleton) => (
                  <button
                    key={singleton.component.component}
                    className={`list-row ${
                      singleton.component.component === state.selectedSingletonComponentId ? "selected" : ""
                    }`}
                    onClick={() =>
                      setState((previous) => ({
                        ...previous,
                        entityPanelTab: "singletons",
                        selectedSingletonComponentId: singleton.component.component
                      }))
                    }
                  >
                    <span>{singleton.component.name || singleton.component.component}</span>
                    <small>{singleton.component.component}</small>
                  </button>
                ))}
                {filteredSingletons.length === 0 && <EmptyInline text="No matching singletons." />}
              </div>
            ) : (
              <div className="list">
                {filteredEntities.map((entity) => (
                  <button
                    key={entity.id}
                    className={`list-row ${entity.id === state.selectedEntityId ? "selected" : ""}`}
                    onClick={() =>
                      setState((previous) => ({
                        ...previous,
                        entityPanelTab: "entities",
                        selectedEntityId: entity.id
                      }))
                    }
                  >
                    <span>{entityLabel(entity)}</span>
                    <small>{entity.id}</small>
                  </button>
                ))}
                {filteredEntities.length === 0 && <EmptyInline text="No matching entities." />}
              </div>
            )}
          </Panel>

          <Panel title="Entity Detail" icon={<SlidersHorizontal size={17} />}>
            {entityPanelTab === "singletons" && selectedSingleton ? (
              <SingletonDetailView
                singleton={selectedSingleton}
                jobs={jobs}
                client={client}
                onSelectJob={(jobId) => setState((previous) => ({ ...previous, selectedJobId: jobId }))}
                onChanged={() => {
                  void refresh();
                }}
              />
            ) : entityPanelTab === "entities" && state.entityDetail ? (
              <EntityDetailView
                entity={state.entityDetail}
                client={client}
                registeredTags={registeredTags}
                onSelectJob={(jobId) => setState((previous) => ({ ...previous, selectedJobId: jobId }))}
                onChanged={() => {
                  void refresh();
                  void loadDetails();
                }}
              />
            ) : (
              <EmptyInline
                text={
                  entityPanelTab === "singletons"
                    ? singletons.length
                      ? "Select a singleton."
                      : "Server has no singletons."
                    : entities.length
                      ? "Select an entity."
                      : "Server has no entities."
                }
              />
            )}
          </Panel>

          <Panel title="Jobs" icon={<Briefcase size={17} />} count={jobs.length}>
            <SearchBox value={jobQuery} onChange={setJobQuery} placeholder="Filter by name, id, order, components..." />
            <div className="split">
              <div className="list compact">
                {filteredJobs.map((job) => (
                  <button
                    key={job.id}
                    className={`list-row ${job.id === state.selectedJobId ? "selected" : ""}`}
                    onClick={() => setState((previous) => ({ ...previous, selectedJobId: job.id }))}
                  >
                    <span>{jobTitle(job)}</span>
                    <small>{job.id}</small>
                  </button>
                ))}
                {filteredJobs.length === 0 && <EmptyInline text="No matching jobs." />}
              </div>
              {state.jobDetail ? (
                <JobDetailView
                  job={state.jobDetail}
                  onSelectEntity={(entityId) =>
                    setState((previous) => ({ ...previous, entityPanelTab: "entities", selectedEntityId: entityId }))
                  }
                  onSelectSingleton={(componentId) =>
                    setState((previous) => ({
                      ...previous,
                      entityPanelTab: "singletons",
                      selectedSingletonComponentId: componentId
                    }))
                  }
                />
              ) : (
                <EmptyInline text={jobs.length ? "Select a job." : "Server has no jobs."} />
              )}
            </div>
          </Panel>
        </main>
      )}
    </div>
  );
}

function readStoredBasePort(): number {
  try {
    const stored = window.localStorage.getItem(BASE_PORT_STORAGE_KEY);
    const parsed = stored ? Number(stored) : DEFAULT_BASE_PORT;
    return validBasePort(parsed) ? parsed : DEFAULT_BASE_PORT;
  } catch {
    return DEFAULT_BASE_PORT;
  }
}

function validBasePort(port: number): boolean {
  return Number.isInteger(port) && port >= 1 && port <= 65532;
}

function Panel(props: { title: string; icon: React.ReactNode; count?: number; children: React.ReactNode }) {
  return (
    <section className="panel">
      <header className="panel-header">
        <span>
          {props.icon}
          {props.title}
        </span>
        {props.count !== undefined && <b>{props.count}</b>}
      </header>
      {props.children}
    </section>
  );
}

function SearchBox(props: { value: string; onChange: (value: string) => void; placeholder: string }) {
  return (
    <label className="search">
      <Search size={15} />
      <input value={props.value} onChange={(event) => props.onChange(event.target.value)} placeholder={props.placeholder} />
    </label>
  );
}

function EmptyInline(props: { icon?: React.ReactNode; text: string }) {
  return (
    <div className="empty-inline">
      {props.icon}
      <span>{props.text}</span>
    </div>
  );
}

function EntityDetailView(props: {
  entity: NonNullable<DebuggerState["entityDetail"]>;
  client?: GraphQLClient;
  registeredTags: RegisteredTag[];
  onSelectJob: (jobId: string) => void;
  onChanged: () => void;
}) {
  return (
    <div className="detail">
      <div className="detail-heading">
        <div>
          <h2>{entityLabel(props.entity)}</h2>
          <code>{props.entity.id}</code>
        </div>
        <span className={`kind ${props.entity.kind.toLowerCase()}`}>{props.entity.kind}</span>
      </div>
      <TagControls
        entityId={props.entity.id}
        components={props.entity.components}
        registeredTags={props.registeredTags}
        client={props.client}
        onChanged={props.onChanged}
      />
      <h3>Components</h3>
      <div className="component-stack">
        {props.entity.components.map((component) => (
          <ComponentCard
            key={component.component}
            entityId={props.entity.id}
            component={component}
            client={props.client}
            onChanged={props.onChanged}
          />
        ))}
        {props.entity.components.length === 0 && <EmptyInline text="No components on this entity." />}
      </div>
      <h3>Matching Jobs</h3>
      <div className="pill-row">
        {props.entity.matchingJobs.map((job) => (
          <button className="pill action-pill" key={job.id} onClick={() => props.onSelectJob(job.id)}>
            <Briefcase size={13} />
            {jobTitle(job)}
          </button>
        ))}
        {props.entity.matchingJobs.length === 0 && <EmptyInline text="No jobs match this entity." />}
      </div>
    </div>
  );
}

function SingletonDetailView(props: {
  singleton: SingletonInstance;
  jobs: JobSummary[];
  client?: GraphQLClient;
  onSelectJob: (jobId: string) => void;
  onChanged: () => void;
}) {
  const matchingJobs = props.jobs.filter((job) =>
    [...job.reads, ...job.writes].some((component) => componentRefId(component) === props.singleton.component.component)
  );
  return (
    <div className="detail">
      <div className="detail-heading">
        <div>
          <h2>{props.singleton.component.name || props.singleton.component.component}</h2>
          <code>{props.singleton.component.component}</code>
        </div>
        <span className="kind system">Singleton</span>
      </div>
      <h3>Component</h3>
      <div className="component-stack">
        <ComponentCard
          entityId={props.singleton.entity.id}
          component={props.singleton.component}
          client={props.client}
          onChanged={props.onChanged}
          allowRemove={false}
        />
      </div>
      <h3>Matching Jobs</h3>
      <div className="pill-row">
        {matchingJobs.map((job) => (
          <button className="pill action-pill" key={job.id} onClick={() => props.onSelectJob(job.id)}>
            <Briefcase size={13} />
            {jobTitle(job)}
          </button>
        ))}
        {matchingJobs.length === 0 && <EmptyInline text="No jobs use this singleton." />}
      </div>
      <h3>Storage Entity</h3>
      <div className="singleton-entity">
        <span>{entityLabel(props.singleton.entity)}</span>
        <code>{props.singleton.entity.id}</code>
      </div>
    </div>
  );
}

function TagControls(props: {
  entityId: string;
  components: ComponentInstance[];
  registeredTags: RegisteredTag[];
  client?: GraphQLClient;
  onChanged: () => void;
}) {
  const registeredTagIds = new Set(props.registeredTags.map((tag) => tag.component));
  const presentTags = props.components.filter((component) => component.tag && registeredTagIds.has(component.component));
  const presentTagIds = new Set(presentTags.map((component) => component.component));
  const missingTags = props.registeredTags.filter((tag) => !presentTagIds.has(tag.component));
  const [busyTag, setBusyTag] = useState<string>();
  const [message, setMessage] = useState<string>();

  async function apply(action: "add" | "remove", component: string) {
    if (!props.client) {
      return;
    }
    setBusyTag(component);
    setMessage(undefined);
    try {
      if (action === "add") {
        await addTag(props.client, props.entityId, component);
      } else {
        await removeComponent(props.client, props.entityId, component);
      }
      props.onChanged();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusyTag(undefined);
    }
  }

  return (
    <section className="tag-manager">
      <h3>Tags</h3>
      <div className="tag-groups">
        <div>
          <h4>On Entity</h4>
          <div className="pill-row">
            {presentTags.map((tag) => (
              <button
                className="pill tag action-pill"
                key={tag.component}
                onClick={() => void apply("remove", tag.component)}
                disabled={busyTag === tag.component || !props.client}
                title="Remove tag"
              >
                <Trash2 size={12} />
                {tag.name || tag.component}
              </button>
            ))}
            {presentTags.length === 0 && <EmptyInline text="No tags on this entity." />}
          </div>
        </div>
        <div>
          <h4>Add Tag</h4>
          <div className="pill-row">
            {missingTags.map((tag) => (
              <button
                className="pill action-pill"
                key={tag.component}
                onClick={() => void apply("add", tag.component)}
                disabled={busyTag === tag.component || !props.client}
                title="Add tag"
              >
                <Plus size={12} />
                {tag.name || tag.component}
              </button>
            ))}
            {missingTags.length === 0 && <EmptyInline text="All registered tags are present." />}
          </div>
        </div>
      </div>
      {message && <p className="inline-error">{message}</p>}
    </section>
  );
}

function ComponentCard(props: {
  entityId: string;
  component: ComponentInstance;
  client?: GraphQLClient;
  onChanged: () => void;
  allowRemove?: boolean;
}) {
  const fields = props.component.fields ?? EMPTY_COMPONENT_FIELDS;
  const [fieldValues, setFieldValues] = useState<Record<string, string | boolean>>(() =>
    componentFieldState(fields)
  );
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string>();

  useEffect(() => {
    setFieldValues(componentFieldState(fields));
  }, [props.component.component, props.component.debugValue, fields]);

  async function remove() {
    if (!props.client) {
      return;
    }
    setBusy(true);
    setMessage(undefined);
    try {
      await removeComponent(props.client, props.entityId, props.component.component);
      props.onChanged();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }

  async function setFieldValue(field: ComponentFieldValue) {
    if (!props.client) {
      return;
    }
    setBusy(true);
    setMessage(undefined);
    try {
      await setComponent(props.client, props.entityId, props.component.component, {
        [field.name]: parseFieldValue(field, fieldValues[field.name])
      });
      props.onChanged();
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }

  return (
    <article className="component-card">
      <div className="component-title">
        <div>
          <strong>{props.component.name || props.component.component}</strong>
          <code>{props.component.component}</code>
        </div>
        <div className="component-actions">
          {props.component.tag && (
            <span className="pill tag">
              <Tag size={12} />
              tag
            </span>
          )}
          {props.component.dirty && <span className="pill dirty">dirty</span>}
          {props.allowRemove !== false && (
            <button className="icon-only" onClick={() => void remove()} disabled={busy} title="Remove component">
              <Trash2 size={15} />
            </button>
          )}
        </div>
      </div>
      <pre>{props.component.debugValue}</pre>
      {!props.component.tag && (
        <div className="value-editor">
          {fields.map((field) => (
            <label className="field-row" key={field.name}>
              <span>
                <strong>{field.name}</strong>
                <small>{field.type}</small>
              </span>
              {field.type === "bool" ? (
                <input
                  type="checkbox"
                  checked={Boolean(fieldValues[field.name])}
                  onChange={(event) =>
                    setFieldValues((previous) => ({ ...previous, [field.name]: event.target.checked }))
                  }
                />
              ) : field.type === "string" ? (
                <input
                  type="text"
                  value={String(fieldValues[field.name] ?? "")}
                  onChange={(event) =>
                    setFieldValues((previous) => ({ ...previous, [field.name]: event.target.value }))
                  }
                />
              ) : (
                <input
                  type="number"
                  step={field.type === "f32" || field.type === "f64" ? "any" : "1"}
                  value={String(fieldValues[field.name] ?? "")}
                  onChange={(event) =>
                    setFieldValues((previous) => ({ ...previous, [field.name]: event.target.value }))
                  }
                />
              )}
              <button onClick={() => void setFieldValue(field)} disabled={busy || !props.client}>
                Save
              </button>
            </label>
          ))}
          {fields.length === 0 && <EmptyInline text="No editable primitive fields." />}
        </div>
      )}
      {message && <p className="inline-error">{message}</p>}
    </article>
  );
}

function componentFieldState(fields: ComponentFieldValue[]): Record<string, string | boolean> {
  return Object.fromEntries(fields.map((field) => [field.name, field.value]));
}

function parseFieldValue(field: ComponentFieldValue, value: string | boolean | undefined): boolean | number | string {
  if (field.type === "bool") {
    return Boolean(value);
  }
  if (field.type === "string") {
    return String(value ?? "");
  }
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${field.name} must be a number`);
  }
  return field.type === "f32" || field.type === "f64" ? parsed : Math.trunc(parsed);
}

function JobDetailView(props: {
  job: NonNullable<DebuggerState["jobDetail"]>;
  onSelectEntity: (entityId: string) => void;
  onSelectSingleton: (componentId: string) => void;
}) {
  return (
    <div className="job-detail">
      <h2>{jobTitle(props.job)}</h2>
      <div className="job-flags">
        <span>{props.job.structural ? "Structural" : "Non-structural"}</span>
        <span>{props.job.singleThread ? "Single thread" : `${props.job.maxThreads} threads`}</span>
        <span>Min chunk {props.job.minEntitiesPerThread}</span>
      </div>
      <h3>Reads</h3>
      <ComponentPills components={props.job.reads} onSelectSingleton={props.onSelectSingleton} />
      <h3>Writes</h3>
      <ComponentPills components={props.job.writes} onSelectSingleton={props.onSelectSingleton} />
      <h3>Matching Entities</h3>
      <div className="list compact">
        {props.job.matchingEntities.map((entity) => (
          <button className="list-row" key={entity.id} onClick={() => props.onSelectEntity(entity.id)}>
            <span>{entityLabel(entity)}</span>
            <small>{entity.id}</small>
          </button>
        ))}
        {props.job.matchingEntities.length === 0 && <EmptyInline text="No matching entities." />}
      </div>
    </div>
  );
}

function ComponentPills(props: { components: Array<ComponentRef | string>; onSelectSingleton?: (componentId: string) => void }) {
  return (
    <div className="pill-row">
      {props.components.map((component) => {
        const componentId = componentRefId(component);
        const singleton = componentRefIsSingleton(component);
        const contents = (
          <>
            {singleton ? <Server size={13} /> : <Activity size={13} />}
            {componentRefName(component)}
          </>
        );
        return singleton && props.onSelectSingleton ? (
          <button className="pill action-pill singleton-pill" key={componentId} onClick={() => props.onSelectSingleton?.(componentId)}>
            {contents}
          </button>
        ) : (
          <span className={`pill ${singleton ? "singleton-pill" : ""}`} key={componentId}>
            {contents}
          </span>
        );
      })}
      {props.components.length === 0 && <EmptyInline text="None." />}
    </div>
  );
}

function matchesEntity(entity: EntitySummary, query: string): boolean {
  const normalized = query.trim().toLowerCase();
  if (!normalized) {
    return true;
  }
  return [entity.id, entity.displayName ?? "", entity.kind, String(entity.index), String(entity.version)].some((value) =>
    value.toLowerCase().includes(normalized)
  );
}

function matchesSingleton(singleton: SingletonInstance, query: string): boolean {
  const normalized = query.trim().toLowerCase();
  if (!normalized) {
    return true;
  }
  return [
    singleton.entity.id,
    singleton.component.component,
    singleton.component.name,
    singleton.component.debugValue,
    String(singleton.entity.index),
    String(singleton.entity.version)
  ].some((value) => value.toLowerCase().includes(normalized));
}

function matchesJob(job: JobSummary, query: string): boolean {
  const normalized = query.trim().toLowerCase();
  if (!normalized) {
    return true;
  }
  return [
    job.id,
    job.name,
    String(job.order),
    ...job.reads.flatMap((component) => [componentRefId(component), componentRefName(component)]),
    ...job.writes.flatMap((component) => [componentRefId(component), componentRefName(component)])
  ].some((value) =>
    value.toLowerCase().includes(normalized)
  );
}

const form = document.querySelector("#appointment-form");
const list = document.querySelector("#appointment-list");
const message = document.querySelector("#form-message");
const saveButton = document.querySelector("#save-button");
const cancelEditButton = document.querySelector("#cancel-edit");
const idField = document.querySelector("#appointment-id");
const authView = document.querySelector("#auth-view");
const appView = document.querySelector("#app-view");
const loginForm = document.querySelector("#login-form");
const loginButton = document.querySelector("#login-button");
const loginMessage = document.querySelector("#login-message");
const adminView = document.querySelector("#admin-view");
const agendaView = document.querySelector("#agenda-view");
const adminTab = document.querySelector("#admin-tab");
const userForm = document.querySelector("#user-form");
const userFormMessage = document.querySelector("#user-form-message");
const userList = document.querySelector("#user-list");
const adminAppointmentList = document.querySelector("#admin-appointment-list");
const createUserButton = document.querySelector("#create-user-button");
let currentUser = null;

function localDateValue() {
  const now = new Date();
  const month = String(now.getMonth() + 1).padStart(2, "0");
  const day = String(now.getDate()).padStart(2, "0");
  return `${now.getFullYear()}-${month}-${day}`;
}

const dateFormatter = new Intl.DateTimeFormat("pt-BR", {
  weekday: "long",
  day: "numeric",
  month: "long",
  year: "numeric",
});

document.querySelector("#today-label").textContent = dateFormatter.format(new Date());
document.querySelector("#date").value = localDateValue();

function showMessage(text, type = "error") {
  message.textContent = text;
  message.className = `message ${type}`;
}

function clearMessage() {
  message.textContent = "";
  message.className = "message hidden";
}

async function request(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { "Content-Type": "application/json", ...options.headers },
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({}));
    if (response.status === 401 && path !== "/api/auth/login") {
      showLogin();
    }
    throw new Error(body.error || "Não foi possível concluir a solicitação.");
  }
  return response.status === 204 ? null : response.json();
}

function showLogin() {
  currentUser = null;
  appView.classList.add("hidden");
  authView.classList.remove("hidden");
  adminTab.classList.add("hidden");
  adminView.classList.add("hidden");
  agendaView.classList.remove("hidden");
  loginMessage.textContent = "";
  loginMessage.className = "message hidden";
}

function showAgenda(user) {
  currentUser = user;
  authView.classList.add("hidden");
  appView.classList.remove("hidden");
  adminTab.classList.toggle("hidden", user.role !== "admin");
  adminView.classList.add("hidden");
  agendaView.classList.remove("hidden");
}

async function showAdmin() {
  if (currentUser?.role !== "admin") return;
  agendaView.classList.add("hidden");
  adminView.classList.remove("hidden");
  await loadAdminData();
}

function createCard(appointment, adminMode = false) {
  const card = document.createElement("article");
  card.className = "appointment-card";

  const time = document.createElement("div");
  time.className = "appointment-time";
  const start = document.createElement("span");
  start.textContent = appointment.startTime;
  const end = document.createElement("span");
  end.textContent = appointment.endTime;
  time.append(start, end);

  const info = document.createElement("div");
  info.className = "appointment-info";
  const title = document.createElement("h3");
  title.textContent = appointment.title;
  const client = document.createElement("p");
  client.textContent = `${appointment.client} · ${new Date(`${appointment.date}T00:00:00`).toLocaleDateString("pt-BR")}`;
  info.append(title, client);
  if (adminMode) {
    const owner = document.createElement("p");
    owner.className = "appointment-notes";
    owner.textContent = `Conta: ${appointment.ownerUsername}`;
    info.append(owner);
  }
  if (appointment.notes) {
    const notes = document.createElement("p");
    notes.className = "appointment-notes";
    notes.textContent = appointment.notes;
    info.append(notes);
  }

  if (adminMode) {
    card.append(time, info);
    return card;
  }

  const actions = document.createElement("div");
  actions.className = "card-actions";
  const editButton = document.createElement("button");
  editButton.className = "icon-button";
  editButton.type = "button";
  editButton.setAttribute("aria-label", `Editar ${appointment.title}`);
  editButton.title = "Editar";
  editButton.textContent = "✎";
  editButton.addEventListener("click", () => beginEdit(appointment));

  const deleteButton = document.createElement("button");
  deleteButton.className = "icon-button delete";
  deleteButton.type = "button";
  deleteButton.setAttribute("aria-label", `Excluir ${appointment.title}`);
  deleteButton.title = "Excluir";
  deleteButton.textContent = "×";
  deleteButton.addEventListener("click", () => deleteAppointment(appointment));
  actions.append(editButton, deleteButton);

  card.append(time, info, actions);
  return card;
}

async function loadAppointments() {
  list.replaceChildren();
  try {
    const appointments = await request("/api/appointments");
    document.querySelector("#appointment-count").textContent = appointments.length;
    if (appointments.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "Ainda não há compromissos. Adicione o primeiro ao lado.";
      list.append(empty);
      return;
    }
    appointments.forEach((appointment) => list.append(createCard(appointment)));
  } catch (error) {
    const empty = document.createElement("p");
    empty.className = "empty-state";
    empty.textContent = error.message;
    list.append(empty);
  }
}

function showAdminError(container, error) {
  const empty = document.createElement("p");
  empty.className = "empty-state";
  empty.textContent = error.message;
  container.replaceChildren(empty);
}

function createUserRow(user) {
  const row = document.createElement("article");
  row.className = "user-row";

  const info = document.createElement("div");
  info.className = "user-info";
  const name = document.createElement("h3");
  name.textContent = user.username;
  const details = document.createElement("p");
  details.textContent = user.role === "admin"
    ? "Administrador"
    : `${user.active ? "Ativo" : "Desativado"} · ${user.appointmentCount} consultas`;
  info.append(name, details);

  row.append(info);
  if (user.role !== "admin") {
    const toggle = document.createElement("button");
    toggle.className = `button ${user.active ? "button-quiet" : "button-primary"} user-toggle`;
    toggle.type = "button";
    toggle.textContent = user.active ? "Desativar" : "Ativar";
    toggle.setAttribute("aria-label", `${user.active ? "Desativar" : "Ativar"} usuário ${user.username}`);
    toggle.addEventListener("click", () => setUserStatus(user, !user.active));
    row.append(toggle);
  }
  return row;
}

async function loadAdminData() {
  userList.replaceChildren();
  adminAppointmentList.replaceChildren();
  try {
    const [users, appointments] = await Promise.all([
      request("/api/admin/users"),
      request("/api/admin/appointments"),
    ]);
    document.querySelector("#user-count").textContent = users.length;
    document.querySelector("#admin-appointment-count").textContent = appointments.length;
    if (users.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "Nenhum usuário cadastrado.";
      userList.append(empty);
    } else {
      users.forEach((user) => userList.append(createUserRow(user)));
    }
    if (appointments.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "Ainda não há consultas agendadas.";
      adminAppointmentList.append(empty);
    } else {
      appointments.forEach((appointment) => adminAppointmentList.append(createCard(appointment, true)));
    }
  } catch (error) {
    showAdminError(userList, error);
    showAdminError(adminAppointmentList, error);
  }
}

async function setUserStatus(user, active) {
  try {
    await request(`/api/admin/users/${user.id}/status`, {
      method: "PUT",
      body: JSON.stringify({ active }),
    });
    await loadAdminData();
  } catch (error) {
    showAdminError(userList, error);
  }
}

function beginEdit(appointment) {
  idField.value = appointment.id;
  form.elements.title.value = appointment.title;
  form.elements.client.value = appointment.client;
  form.elements.date.value = appointment.date;
  form.elements.startTime.value = appointment.startTime;
  form.elements.endTime.value = appointment.endTime;
  form.elements.notes.value = appointment.notes;
  document.querySelector("#form-heading").textContent = "Editar compromisso";
  saveButton.textContent = "Salvar alterações";
  cancelEditButton.classList.remove("hidden");
  clearMessage();
  form.elements.title.focus();
}

function resetForm() {
  form.reset();
  idField.value = "";
  form.elements.date.value = localDateValue();
  document.querySelector("#form-heading").textContent = "Novo compromisso";
  saveButton.textContent = "Salvar compromisso";
  cancelEditButton.classList.add("hidden");
  clearMessage();
}

async function deleteAppointment(appointment) {
  if (!window.confirm(`Excluir o compromisso "${appointment.title}"?`)) return;
  try {
    await request(`/api/appointments/${appointment.id}`, { method: "DELETE" });
    if (idField.value === String(appointment.id)) resetForm();
    await loadAppointments();
  } catch (error) {
    showMessage(error.message);
  }
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  clearMessage();

  const appointment = Object.fromEntries(new FormData(form).entries());
  delete appointment.id;
  const id = idField.value;
  const path = id ? `/api/appointments/${id}` : "/api/appointments";
  const method = id ? "PUT" : "POST";
  saveButton.disabled = true;

  try {
    await request(path, { method, body: JSON.stringify(appointment) });
    resetForm();
    showMessage(id ? "Compromisso atualizado." : "Compromisso salvo.", "success");
    await loadAppointments();
  } catch (error) {
    showMessage(error.message);
  } finally {
    saveButton.disabled = false;
  }
});

cancelEditButton.addEventListener("click", resetForm);

loginForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  loginButton.disabled = true;
  loginMessage.textContent = "";
  loginMessage.className = "message hidden";

  const credentials = Object.fromEntries(new FormData(loginForm).entries());
  try {
    const user = await request("/api/auth/login", {
      method: "POST",
      body: JSON.stringify(credentials),
    });
    loginForm.reset();
    showAgenda(user);
    await loadAppointments();
  } catch (error) {
    loginMessage.textContent = error.message;
    loginMessage.className = "message error";
  } finally {
    loginButton.disabled = false;
  }
});

document.querySelector("#logout-button").addEventListener("click", async () => {
  try {
    await request("/api/auth/logout", { method: "POST" });
    showLogin();
  } catch (error) {
    showMessage(error.message);
  }
});

adminTab.addEventListener("click", showAdmin);
document.querySelector("#back-to-agenda").addEventListener("click", () => {
  adminView.classList.add("hidden");
  agendaView.classList.remove("hidden");
});

userForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  createUserButton.disabled = true;
  userFormMessage.textContent = "";
  userFormMessage.className = "message hidden";
  const user = Object.fromEntries(new FormData(userForm).entries());
  try {
    await request("/api/admin/users", {
      method: "POST",
      body: JSON.stringify(user),
    });
    userForm.reset();
    userFormMessage.textContent = "Usuário criado. A conta já pode entrar na agenda.";
    userFormMessage.className = "message success";
    await loadAdminData();
  } catch (error) {
    userFormMessage.textContent = error.message;
    userFormMessage.className = "message error";
  } finally {
    createUserButton.disabled = false;
  }
});

async function restoreSession() {
  try {
    const user = await request("/api/auth/session");
    showAgenda(user);
    await loadAppointments();
  } catch (error) {
    showLogin();
    if (!error.message.includes("Faça login")) {
      loginMessage.textContent = error.message;
      loginMessage.className = "message error";
    }
  }
}

restoreSession();

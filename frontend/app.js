const form = document.querySelector("#appointment-form");
const list = document.querySelector("#appointment-list");
const message = document.querySelector("#form-message");
const specialtySelect = document.querySelector("#specialty-select");
const availabilitySelect = document.querySelector("#availability-slot");
const saveButton = document.querySelector("#save-button");
const cancelEditButton = document.querySelector("#cancel-edit");
const idField = document.querySelector("#appointment-id");
const authView = document.querySelector("#auth-view");
const appView = document.querySelector("#app-view");
const loginForm = document.querySelector("#login-form");
const loginButton = document.querySelector("#login-button");
const loginMessage = document.querySelector("#login-message");
const registerForm = document.querySelector("#register-form");
const registerButton = document.querySelector("#register-button");
const registerMessage = document.querySelector("#register-message");
const loginTab = document.querySelector("#login-tab");
const registerTab = document.querySelector("#register-tab");
const authTitle = document.querySelector("#auth-title");
const authCopy = document.querySelector("#auth-copy");
const adminView = document.querySelector("#admin-view");
const agendaView = document.querySelector("#agenda-view");
const notificationButton = document.querySelector("#notification-button");
const notificationPanel = document.querySelector("#notification-panel");
const notificationList = document.querySelector("#notification-list");
const notificationCount = document.querySelector("#notification-count");
const paymentTypeSelect = document.querySelector("#payment-type");
const insuranceField = document.querySelector("#insurance-field");
const insuranceNameInput = document.querySelector("#insurance-name");
const availabilityForm = document.querySelector("#availability-form");
const availabilityFormMessage = document.querySelector("#availability-form-message");
const availabilitySaveButton = document.querySelector("#availability-save-button");
const adminAvailabilityList = document.querySelector("#admin-availability-list");
const adminSpecialtySelect = document.querySelector("#availability-specialty");
const customSpecialtyField = document.querySelector("#custom-specialty-field");
const customSpecialtyInput = document.querySelector("#custom-specialty");
let currentUser = null;
let availabilitySlots = [];
let adminAvailabilitySlots = [];
let editingReservedSlot = null;
let notificationRefreshTimer = null;

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
document.querySelector("#availability-date").value = localDateValue();

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
    const body = await response.json().catch(() => null);
    if (response.status === 401 && path !== "/api/auth/login") {
      showLogin();
    }
    const message = body?.error ||
      (response.status === 404 && path.startsWith("/api/")
        ? "O servidor aberto não reconhece esta função. Feche-o com Ctrl+C e reinicie pelo scripts\\start.ps1."
        : response.status === 404
          ? "A página ou o recurso solicitado não foi encontrado."
        : `Não foi possível concluir a solicitação (HTTP ${response.status}).`);
    throw new Error(message);
  }
  return response.status === 204 ? null : response.json();
}

function showLogin() {
  if (notificationRefreshTimer !== null) {
    window.clearInterval(notificationRefreshTimer);
    notificationRefreshTimer = null;
  }
  currentUser = null;
  appView.classList.add("hidden");
  authView.classList.remove("hidden");
  notificationPanel.classList.add("hidden");
  notificationButton.setAttribute("aria-expanded", "false");
  notificationCount.classList.add("hidden");
  adminView.classList.add("hidden");
  agendaView.classList.remove("hidden");
  loginMessage.textContent = "";
  loginMessage.className = "message hidden";
  registerMessage.textContent = "";
  registerMessage.className = "message hidden";
  selectAuthMode("login");
}

function selectAuthMode(mode) {
  const registering = mode === "register";
  loginForm.classList.toggle("hidden", registering);
  registerForm.classList.toggle("hidden", !registering);
  loginTab.classList.toggle("active", !registering);
  registerTab.classList.toggle("active", registering);
  loginTab.setAttribute("aria-selected", String(!registering));
  registerTab.setAttribute("aria-selected", String(registering));
  authTitle.textContent = registering ? "Crie sua conta." : "Bem-vindo de volta.";
  authCopy.textContent = registering
    ? "Cadastre-se para organizar seus compromissos em um espaço só seu."
    : "Entre com seu usuário e sua senha para acessar seus compromissos.";
}

function showAgenda(user) {
  currentUser = user;
  authView.classList.add("hidden");
  appView.classList.remove("hidden");
  const isAdmin = user.role === "admin";
  adminView.classList.toggle("hidden", !isAdmin);
  agendaView.classList.toggle("hidden", isAdmin);
}

async function loadViewData(user) {
  const dataLoad = user.role === "admin"
    ? loadAdminData()
    : Promise.all([loadAppointments(), loadAvailability()]);
  await Promise.all([dataLoad, loadNotifications()]);
  if (notificationRefreshTimer !== null) {
    window.clearInterval(notificationRefreshTimer);
  }
  notificationRefreshTimer = window.setInterval(() => {
    if (currentUser) loadNotifications();
  }, 60_000);
}

function formatPaymentType(appointment) {
  return appointment.paymentType === "convenio"
    ? `Convênio: ${appointment.insuranceName}`
    : "Particular";
}

function formatNotificationDate(notification) {
  const date = new Date(`${notification.date}T00:00:00`).toLocaleDateString("pt-BR");
  return `${date} · ${notification.startTime}`;
}

function renderNotifications(notifications) {
  notificationList.replaceChildren();
  const isAdmin = currentUser?.role === "admin";
  const count = isAdmin
    ? notifications.length
    : notifications.filter((notification) => notification.status === "pending").length;
  notificationCount.textContent = String(count);
  notificationCount.classList.toggle("hidden", count === 0);
  notificationButton.setAttribute(
    "aria-label",
    count > 0 ? `Notificações, ${count} novas` : "Notificações");

  if (notifications.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty-state";
    empty.textContent = isAdmin
      ? "Nenhuma consulta confirmada recentemente."
      : "Nenhuma consulta precisa de confirmação agora.";
    notificationList.append(empty);
    return;
  }

  notifications.forEach((notification) => {
    const item = document.createElement("article");
    item.className = "notification-item";
    const heading = document.createElement("h3");
    heading.textContent = isAdmin
      ? `Consulta confirmada por ${notification.patientUsername}`
      : notification.status === "confirmed"
        ? "Consulta confirmada"
        : "Confirme sua consulta";
    const details = document.createElement("p");
    const professional = notification.professionalName
      ? ` · ${notification.professionalName}`
      : "";
    details.textContent =
      `${notification.specialty}${professional} · ${formatNotificationDate(notification)}`;
    const payment = document.createElement("p");
    payment.textContent = notification.paymentType === "convenio"
      ? `Convênio: ${notification.insuranceName}`
      : "Particular";
    item.append(heading, details, payment);

    if (isAdmin) {
      const patient = document.createElement("p");
      patient.textContent = `Paciente: ${notification.patientUsername}`;
      item.append(patient);
    } else if (notification.status === "pending") {
      const confirmButton = document.createElement("button");
      confirmButton.className = "button button-primary";
      confirmButton.type = "button";
      confirmButton.textContent = "Confirmar consulta";
      confirmButton.addEventListener("click", () => confirmAppointment(notification, confirmButton));
      item.append(confirmButton);
    } else {
      const status = document.createElement("p");
      status.textContent = "Sua presença está confirmada.";
      item.append(status);
    }

    notificationList.append(item);
  });
}

async function loadNotifications() {
  try {
    const notifications = await request("/api/notifications");
    renderNotifications(notifications);
  } catch (error) {
    if (!currentUser) return;
    notificationList.replaceChildren();
    const failure = document.createElement("p");
    failure.className = "empty-state";
    failure.textContent = error.message;
    notificationList.append(failure);
  }
}

async function confirmAppointment(notification, button) {
  button.disabled = true;
  try {
    await request(`/api/appointments/${notification.appointmentId}/confirm`, { method: "POST" });
    await Promise.all([loadNotifications(), loadAppointments()]);
  } catch (error) {
    button.disabled = false;
    const failure = document.createElement("p");
    failure.className = "message error";
    failure.textContent = error.message;
    button.after(failure);
  }
}

function createCard(appointment) {
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
  title.textContent = appointment.specialty || appointment.title;
  const client = document.createElement("p");
  client.textContent = new Date(`${appointment.date}T00:00:00`).toLocaleDateString("pt-BR");
  info.append(title, client);
  if (appointment.professionalName) {
    const professional = document.createElement("p");
    professional.textContent = `Profissional: ${appointment.professionalName}`;
    info.append(professional);
  }
  const payment = document.createElement("p");
  payment.textContent = formatPaymentType(appointment);
  info.append(payment);
  if (appointment.notes) {
    const notes = document.createElement("p");
    notes.className = "appointment-notes";
    notes.textContent = appointment.notes;
    info.append(notes);
  }
  if (appointment.specialty) {
    const specialty = document.createElement("span");
    specialty.className = "specialty-badge";
    specialty.textContent = appointment.specialty;
    info.append(specialty);
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

function formatAvailabilityLabel(slot) {
  const date = new Date(`${slot.date}T00:00:00`).toLocaleDateString("pt-BR");
  return `${date} · ${slot.startTime}–${slot.endTime}`;
}

function formatAvailabilityOption(slot) {
  const professional = slot.professionalName ? `${slot.professionalName} · ` : "";
  return `${professional}${formatAvailabilityLabel(slot)}`;
}

function renderAvailabilityOptions(selectedSlotId = "") {
  availabilitySelect.replaceChildren();
  const specialty = specialtySelect.value;
  const slots = availabilitySlots.filter((slot) =>
    slot.specialty === specialty);
  if (editingReservedSlot &&
      editingReservedSlot.specialty === specialty &&
      !slots.some((slot) => slot.id === editingReservedSlot.id)) {
    slots.push(editingReservedSlot);
  }
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.disabled = true;
  placeholder.textContent = slots.length > 0
    ? "Selecione um horário"
    : specialty
      ? "Peça ao Admin para cadastrar novos horários"
      : "Escolha primeiro a especialidade";
  availabilitySelect.append(placeholder);
  slots.forEach((slot) => {
    const option = document.createElement("option");
    option.value = String(slot.id);
    option.textContent = formatAvailabilityOption(slot);
    availabilitySelect.append(option);
  });
  availabilitySelect.value = String(selectedSlotId || "");
  availabilitySelect.disabled = !specialty || slots.length === 0;
  saveButton.disabled = slots.length === 0;
}

function renderSpecialtyOptions(selectedSpecialty = "") {
  specialtySelect.replaceChildren();
  const slots = [...availabilitySlots];
  if (editingReservedSlot && !slots.some((slot) => slot.id === editingReservedSlot.id)) {
    slots.push(editingReservedSlot);
  }
  const defaultSpecialties = [
    "Dermatologista",
    "Cardiologista",
    "Cirurgião plástico",
  ];
  const specialties = [...defaultSpecialties];
  slots.forEach(({ specialty }) => {
    if (specialty && !specialties.some((available) =>
      available.localeCompare(specialty, "pt-BR", { sensitivity: "base" }) === 0)) {
      specialties.push(specialty);
    }
  });
  specialties.sort((first, second) => first.localeCompare(second, "pt-BR"));
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.disabled = true;
  placeholder.textContent = specialties.length > 0
    ? "Selecione uma especialidade"
    : "Nenhuma especialidade com horário disponível";
  specialtySelect.append(placeholder);
  specialties.forEach((specialty) => {
    const option = document.createElement("option");
    option.value = specialty;
    option.textContent = specialty;
    specialtySelect.append(option);
  });
  specialtySelect.disabled = false;
  specialtySelect.value = selectedSpecialty;
  renderAvailabilityOptions();
}

async function loadAvailability(selectedSlotId = "") {
  try {
    availabilitySlots = await request("/api/availability");
    const selectedSlot = availabilitySlots.find((slot) => slot.id === Number(selectedSlotId));
    renderSpecialtyOptions(selectedSlot?.specialty || "");
    if (selectedSlot) renderAvailabilityOptions(selectedSlot.id);
  } catch (error) {
    availabilitySlots = [];
    renderSpecialtyOptions();
    specialtySelect.disabled = true;
    availabilitySelect.disabled = true;
    const option = document.createElement("option");
    option.value = "";
    option.textContent = error.message;
    availabilitySelect.replaceChildren(option);
    showMessage(error.message);
  }
}

async function loadAppointments() {
  list.replaceChildren();
  try {
    const appointments = await request("/api/appointments");
    document.querySelector("#appointment-count").textContent = appointments.length;
    if (appointments.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "Ainda não há compromissos. Escolha um horário disponível ao lado para reservar.";
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

function createAvailabilityRow(slot) {
  const row = document.createElement("article");
  row.className = `user-row${slot.isBooked ? " is-booked" : ""}`;

  const info = document.createElement("div");
  info.className = "user-info";
  const label = document.createElement("h3");
  label.textContent = slot.specialty;
  const professional = document.createElement("p");
  professional.textContent = `Profissional: ${slot.professionalName || "Não informado"}`;
  const time = document.createElement("p");
  time.textContent = formatAvailabilityLabel(slot);
  const status = document.createElement("p");
  status.textContent = slot.isBooked
    ? `Agendada por: ${slot.bookedBy || "Conta não identificada"}`
    : "Disponível para qualquer paciente";
  info.append(label, professional, time, status);
  row.append(info);

  if (!slot.isBooked) {
    const removeButton = document.createElement("button");
    removeButton.className = "button button-quiet user-toggle";
    removeButton.type = "button";
    removeButton.textContent = "Remover";
    removeButton.setAttribute("aria-label", `Remover horário ${formatAvailabilityLabel(slot)}`);
    removeButton.addEventListener("click", () => deleteAvailability(slot));
    row.append(removeButton);
  }
  return row;
}

async function loadAdminData() {
  adminAvailabilityList.replaceChildren();
  try {
    const slots = await request("/api/admin/availability");
    adminAvailabilitySlots = slots;
    document.querySelector("#availability-count").textContent = slots.length;
    if (slots.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty-state";
      empty.textContent = "Nenhum horário cadastrado. Adicione uma especialidade e um horário para começar.";
      adminAvailabilityList.append(empty);
    } else {
      slots.forEach((slot) => adminAvailabilityList.append(createAvailabilityRow(slot)));
    }
  } catch (error) {
    showAdminError(adminAvailabilityList, error);
  }
}

async function deleteAvailability(slot) {
  if (!window.confirm(`Remover o horário ${formatAvailabilityLabel(slot)}?`)) return;
  try {
    await request(`/api/admin/availability/${slot.id}`, { method: "DELETE" });
    await Promise.all([loadAdminData(), loadAvailability()]);
  } catch (error) {
    showAdminError(adminAvailabilityList, error);
  }
}

function beginEdit(appointment) {
  idField.value = appointment.id;
  form.elements.notes.value = appointment.notes;
  paymentTypeSelect.value = appointment.paymentType || "particular";
  insuranceNameInput.value = appointment.insuranceName || "";
  updateInsuranceField();
  editingReservedSlot = appointment.availabilitySlotId
    ? {
        id: appointment.availabilitySlotId,
        date: appointment.date,
        startTime: appointment.startTime,
        endTime: appointment.endTime,
        specialty: appointment.specialty,
        professionalName: appointment.professionalName,
      }
    : null;
  renderSpecialtyOptions(appointment.specialty);
  renderAvailabilityOptions(appointment.availabilitySlotId);
  document.querySelector("#form-heading").textContent = "Editar consulta";
  saveButton.textContent = "Salvar alterações";
  cancelEditButton.classList.remove("hidden");
  clearMessage();
  specialtySelect.focus();
}

function resetForm() {
  form.reset();
  updateInsuranceField();
  idField.value = "";
  editingReservedSlot = null;
  renderSpecialtyOptions();
  document.querySelector("#form-heading").textContent = "Agendar consulta";
  saveButton.textContent = "Agendar consulta";
  cancelEditButton.classList.add("hidden");
  clearMessage();
}

async function deleteAppointment(appointment) {
  if (!window.confirm(`Cancelar a consulta de ${appointment.specialty || "medicina"}?`)) return;
  try {
    await request(`/api/appointments/${appointment.id}`, { method: "DELETE" });
    if (idField.value === String(appointment.id)) resetForm();
    await Promise.all([loadAppointments(), loadAvailability()]);
  } catch (error) {
    showMessage(error.message);
  }
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  clearMessage();

  const formValues = Object.fromEntries(new FormData(form).entries());
  const selectedSlotId = Number(formValues.availabilitySlotId);
  const slot = availabilitySlots.find((availableSlot) =>
    availableSlot.id === selectedSlotId &&
    availableSlot.specialty === specialtySelect.value) ||
    (editingReservedSlot?.id === selectedSlotId ? editingReservedSlot : null);
  if (!slot) {
    showMessage("Escolha um horário disponibilizado pelo administrador.");
    return;
  }
  const appointment = {
    date: slot.date,
    startTime: slot.startTime,
    endTime: slot.endTime,
    notes: formValues.notes,
    availabilitySlotId: selectedSlotId,
    paymentType: formValues.paymentType,
    insuranceName: String(formValues.insuranceName || "").trim(),
  };
  const id = idField.value;
  const path = id ? `/api/appointments/${id}` : "/api/appointments";
  const method = id ? "PUT" : "POST";
  saveButton.disabled = true;

  try {
    await request(path, { method, body: JSON.stringify(appointment) });
    resetForm();
    showMessage(id ? "Consulta atualizada." : "Consulta agendada.", "success");
    await Promise.all([loadAppointments(), loadAvailability()]);
  } catch (error) {
    showMessage(error.message);
  } finally {
    saveButton.disabled = availabilitySlots.length + (editingReservedSlot ? 1 : 0) === 0;
  }
});

cancelEditButton.addEventListener("click", resetForm);

specialtySelect.addEventListener("change", () => {
  renderAvailabilityOptions();
});

function updateInsuranceField() {
  const usingInsurance = paymentTypeSelect.value === "convenio";
  insuranceField.classList.toggle("hidden", !usingInsurance);
  insuranceNameInput.required = usingInsurance;
}

paymentTypeSelect.addEventListener("change", updateInsuranceField);
updateInsuranceField();

notificationButton.addEventListener("click", async () => {
  const willOpen = notificationPanel.classList.contains("hidden");
  notificationPanel.classList.toggle("hidden", !willOpen);
  notificationButton.setAttribute("aria-expanded", String(willOpen));
  if (willOpen) await loadNotifications();
});

document.querySelector("#notification-close").addEventListener("click", () => {
  notificationPanel.classList.add("hidden");
  notificationButton.setAttribute("aria-expanded", "false");
});

document.addEventListener("click", (event) => {
  if (!event.target.closest(".notification-wrapper")) {
    notificationPanel.classList.add("hidden");
    notificationButton.setAttribute("aria-expanded", "false");
  }
});

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
    await loadViewData(user);
  } catch (error) {
    loginMessage.textContent = error.message;
    loginMessage.className = "message error";
  } finally {
    loginButton.disabled = false;
  }
});

loginTab.addEventListener("click", () => selectAuthMode("login"));
registerTab.addEventListener("click", () => selectAuthMode("register"));

registerForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  registerMessage.textContent = "";
  registerMessage.className = "message hidden";

  const formData = new FormData(registerForm);
  const username = String(formData.get("username") || "").trim();
  const password = String(formData.get("password") || "");
  const passwordConfirm = String(formData.get("passwordConfirm") || "");
  if (password !== passwordConfirm) {
    registerMessage.textContent = "As senhas não conferem.";
    registerMessage.className = "message error";
    return;
  }

  registerButton.disabled = true;
  try {
    const user = await request("/api/auth/register", {
      method: "POST",
      body: JSON.stringify({ username, password }),
    });
    registerForm.reset();
    showAgenda(user);
    await loadViewData(user);
  } catch (error) {
    registerMessage.textContent = error.message;
    registerMessage.className = "message error";
  } finally {
    registerButton.disabled = false;
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

document.querySelector("#back-to-agenda").addEventListener("click", () => {
  adminView.classList.add("hidden");
  agendaView.classList.remove("hidden");
});

availabilityForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  availabilitySaveButton.disabled = true;
  availabilityFormMessage.textContent = "";
  availabilityFormMessage.className = "message hidden";
  const slot = Object.fromEntries(new FormData(availabilityForm).entries());
  if (slot.specialty === "__other__") {
    slot.specialty = customSpecialtyInput.value.trim();
    if (!slot.specialty) {
      customSpecialtyInput.setCustomValidity("Informe a especialidade.");
      customSpecialtyInput.reportValidity();
      availabilitySaveButton.disabled = false;
      return;
    }
  }
  slot.professionalName = String(slot.professionalName || "").trim();
  customSpecialtyInput.setCustomValidity("");
  try {
    await request("/api/admin/availability", {
      method: "POST",
      body: JSON.stringify(slot),
    });
    availabilityForm.reset();
    customSpecialtyField.classList.add("hidden");
    customSpecialtyInput.required = false;
    document.querySelector("#availability-date").value = localDateValue();
    availabilityFormMessage.textContent = "Horário cadastrado e disponível para reserva.";
    availabilityFormMessage.className = "message success";
    await Promise.all([loadAdminData(), loadAvailability()]);
  } catch (error) {
    availabilityFormMessage.textContent = error.message;
    availabilityFormMessage.className = "message error";
  } finally {
    availabilitySaveButton.disabled = false;
  }
});

adminSpecialtySelect.addEventListener("change", () => {
  customSpecialtyInput.setCustomValidity("");
  const customSpecialty = adminSpecialtySelect.value === "__other__";
  customSpecialtyField.classList.toggle("hidden", !customSpecialty);
  customSpecialtyInput.required = customSpecialty;
  if (customSpecialty) customSpecialtyInput.focus();
});

async function restoreSession() {
  try {
    const user = await request("/api/auth/session");
    showAgenda(user);
    await loadViewData(user);
  } catch (error) {
    showLogin();
    if (!error.message.includes("Faça login")) {
      loginMessage.textContent = error.message;
      loginMessage.className = "message error";
    }
  }
}

restoreSession();

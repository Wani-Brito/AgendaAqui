# AgendaAqui

Aplicação local de agenda de compromissos, com backend em C++, interface HTML/CSS/JavaScript e persistência em SQLite.

## Requisitos

- Windows 10/11
- CMake 3.25 ou superior
- Compilador C++17. No Windows, instale o Visual Studio ou Build Tools com a carga de trabalho **Desenvolvimento para desktop com C++**.
- Acesso à internet na primeira configuração para baixar as bibliotecas C++ declaradas no CMake.

> Este projeto usa C++, portanto não precisa de `venv`. Ambientes virtuais (`venv`) são específicos para dependências Python. O banco SQLite é criado automaticamente na primeira execução.

## Compilador C++ no Windows

Se o compilador ainda não estiver instalado, abra o **Visual Studio Installer**, clique em **Modificar** na instalação **Build Tools for Visual Studio 2022** e marque **Desenvolvimento para desktop com C++**. Confirme também que um MSVC v143 e um Windows SDK estão selecionados nos componentes da carga de trabalho. Não tente instalar os Build Tools novamente pelo `winget` se já aparecerem no Installer.

## Iniciar no Windows

No PowerShell, na pasta do projeto:

```powershell
.\scripts\start.ps1
```

Se uma versão anterior do AgendaAqui estiver aberta, volte ao terminal dela e pressione **Ctrl+C** antes de iniciar a nova versão. Na primeira execução, o CMake baixa cpp-httplib, nlohmann/json e SQLite; em seguida compila o servidor. Acesse <http://localhost:8080>. Se essa porta já estiver ocupada, configure outra antes de executar o script, por exemplo `$env:AGENDA_PORT = "8081"`.

Na primeira execução, o sistema cria as contas padrão `admin` / `adminpassword` e `usercomum` / `usercompassword`. Na tela inicial, use **Entrar** para acessar uma conta existente ou **Criar conta** para cadastrar um novo usuário comum e entrar automaticamente. O cadastro exige usuário de 3 a 64 caracteres (letras, números, ponto, hífen ou sublinhado) e senha de pelo menos 12 caracteres. Contas novas e suas senhas são salvas na tabela `users` do SQLite; o cadastro público nunca cria administradores.

Conforme solicitado para esta versão, as senhas ficam como texto no SQLite, na coluna `users.password`; isso é inseguro se alguém copiar o arquivo do banco, então mantenha o sistema apenas no computador local e proteja o arquivo. O acesso à agenda usa uma sessão temporária em cookie `HttpOnly`, e as rotas da API exigem login.

## Perfis e painel Admin

A conta padrão `admin` tem perfil **administrador**. Na aba **Admin**, o administrador pode cadastrar horários da clínica escolhendo uma especialidade (Dermatologista, Cardiologista, Cirurgião plástico ou “Outros” com texto personalizado), informando o nome do profissional, a data e o intervalo; esses horários ficam disponíveis para qualquer paciente, sem vincular a agenda antecipadamente a uma pessoa ou consulta específica. A lista de horários mostra o profissional e se cada vaga está disponível ou agendada e, após a reserva, o usuário da conta do paciente. O administrador também pode criar outras contas comuns e ativar/desativar acessos. A conta `usercomum` já existe para testar; ela também pode ser criada pela aba **Criar conta** caso ainda não exista. Cada paciente agenda a especialidade e o horário no próprio perfil; a consulta aparece somente na agenda da conta que a reservou.

Ao reservar, o paciente informa se o atendimento será **Particular** ou por **Convênio**; para convênio, também informa o nome. O sino mostra lembretes a partir de 24 horas antes da consulta (imediatamente para agendamentos feitos com menos de 24 horas). O paciente confirma no sino; o administrador vê a confirmação, o nome da conta do paciente e o tipo de atendimento. Os lembretes são atualizados enquanto a aplicação está aberta.

As contas comuns veem e gerenciam somente os próprios compromissos, não veem o botão Admin e não podem abrir os endpoints administrativos. Ao desativar uma conta, as sessões dela são revogadas. A interface não permite desativar a conta administradora.

O banco é migrado automaticamente ao iniciar; horários antigos mantêm a especialidade já salva e nomes que estejam na coluna legada de médico são copiados para o campo de profissional. Compromissos existentes antes da separação por usuário são associados à conta administradora já existente. Contas de versões anteriores que tinham senha protegida continuam podendo entrar; após o primeiro login, a senha é copiada para a coluna `password` e o campo legado é limpo. Para aplicar atualizações, encerre o servidor em execução com **Ctrl+C** e execute `.\scripts\start.ps1` no PowerShell; o script compila e inicia a versão atual. O banco fica em `data/agendaaqui.db` e armazena contas, senhas, sessões, horários disponíveis e compromissos.

O servidor está configurado para uso local em `localhost`; não o exponha à internet sem HTTPS e configuração adicional de segurança.

Também é possível executar manualmente:

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
.\build-vs\Release\agendaaqui.exe
```

## Banco de dados

O esquema está em [`database/schema.sql`](database/schema.sql). A aplicação o aplica ao iniciar e cria `data/agendaaqui.db`. Para mudar o caminho do arquivo:

```powershell
$env:AGENDA_DB_PATH = "C:\caminho\para\agendaaqui.db"
.\scripts\start.ps1
```

## API

Todos os endpoints usam JSON:

- `GET /api/appointments` — listar compromissos
- `GET /api/notifications` — listar lembretes do paciente ou confirmações recentes para o administrador
- `GET /api/availability` — listar horários disponíveis para reserva
- `POST /api/auth/register` — criar conta comum e iniciar sessão
- `POST /api/appointments` — criar compromisso
- `POST /api/appointments/{id}/confirm` — confirmar a consulta do paciente nas 24 horas anteriores
- `PUT /api/appointments/{id}` — editar compromisso
- `DELETE /api/appointments/{id}` — excluir compromisso
- `GET /api/admin/appointments` — listar todos os compromissos (administrador)
- `GET /api/admin/availability` — listar horários cadastrados (administrador)
- `POST /api/admin/availability` — cadastrar um horário disponível (administrador)
- `DELETE /api/admin/availability/{id}` — remover um horário ainda não reservado (administrador)
- `GET /api/admin/users` — listar contas (administrador)
- `POST /api/admin/users` — criar conta comum (administrador)
- `PUT /api/admin/users/{id}/status` — ativar/desativar conta e revogar sessões (administrador)

Para reservar, o paciente escolhe a especialidade e envia `availabilitySlotId`, `date` (`AAAA-MM-DD`), `startTime` e `endTime` (`HH:MM`); a data e os horários devem corresponder à vaga escolhida e são validados pelo servidor. O sistema associa a consulta automaticamente à conta autenticada, sem solicitar nome de paciente. `paymentType` aceita `particular` ou `convenio`; para `convenio`, `insuranceName` é obrigatório. `notes` é opcional. O administrador cadastra horários com `specialty`, `professionalName`, `date`, `startTime` e `endTime`. A lista administrativa de disponibilidade mostra o profissional, o estado e a conta do paciente após a reserva; o endpoint público retorna somente horários livres. O servidor impede sobreposições para o mesmo profissional e reservas duplicadas; um conflito retorna HTTP 409. As confirmações ficam registradas em `confirmed_at` e aparecem no sino do administrador por sete dias.

## Estrutura

```text
backend/       Servidor C++ e API REST
database/      Esquema SQL do SQLite
frontend/      Interface responsiva HTML, CSS e JavaScript
scripts/       Inicialização do projeto no Windows
```

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

Na primeira execução, o sistema cria as contas padrão `admin` / `adminpassword` e `usercomum` / `usercompassword`. Entre com uma delas na tela de login. Conforme solicitado para esta versão, as senhas ficam como texto no SQLite, na coluna `users.password`; isso é inseguro se alguém copiar o arquivo do banco, então mantenha o sistema apenas no computador local e proteja o arquivo. O acesso à agenda usa uma sessão temporária em cookie `HttpOnly`, e as rotas da API exigem login.

## Perfis e painel Admin

A conta padrão `admin` tem perfil **administrador**. Na aba **Admin**, o administrador pode criar outras contas comuns, ativar/desativar acessos e consultar todos os compromissos, incluindo o nome do usuário responsável. A conta `usercomum` já existe para testar: ela pode ver e gerenciar apenas os próprios compromissos. O administrador também pode editar ou excluir compromissos.

As contas comuns veem e gerenciam somente os próprios compromissos e não podem abrir os endpoints administrativos. Ao desativar uma conta, as sessões dela são revogadas. A interface não permite desativar a conta administradora.

O banco é migrado automaticamente ao iniciar. Compromissos existentes antes da separação por usuário são associados à conta administradora já existente. Contas de versões anteriores que tinham senha protegida continuam podendo entrar; após o primeiro login, a senha é copiada para a coluna `password` e o campo legado é limpo. Para reiniciar o servidor, execute `.\scripts\start.ps1` no PowerShell. O banco fica em `data/agendaaqui.db` e armazena contas, senhas, sessões e compromissos.

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
- `POST /api/appointments` — criar compromisso
- `PUT /api/appointments/{id}` — editar compromisso
- `DELETE /api/appointments/{id}` — excluir compromisso
- `GET /api/admin/appointments` — listar todos os compromissos (administrador)
- `GET /api/admin/users` — listar contas (administrador)
- `POST /api/admin/users` — criar conta comum (administrador)
- `PUT /api/admin/users/{id}/status` — ativar/desativar conta e revogar sessões (administrador)

Campos obrigatórios: `title`, `client`, `date` (`AAAA-MM-DD`), `startTime` e `endTime` (`HH:MM`). `notes` é opcional. O servidor valida os campos e impede intervalos sobrepostos no mesmo dia; uma sobreposição retorna HTTP 409.

## Estrutura

```text
backend/       Servidor C++ e API REST
database/      Esquema SQL do SQLite
frontend/      Interface responsiva HTML, CSS e JavaScript
scripts/       Inicialização do projeto no Windows
```

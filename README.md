# 🌱 Granja - Monitoramento com ESP32 (MQTT)

Projeto desenvolvido com **ESP-IDF** para monitoramento e controle utilizando um **ESP32** com comunicação via **MQTT**.

O firmware realiza leitura de sensores e controle de atuadores, enviando dados para um broker MQTT para integração com sistemas de automação, dashboards ou backends.

## Funcionalidades

- Leitura de sensores (temperatura, umidade, etc.)
- Controle de atuadores (ventiladores, luzes, bombas, relés)
- Comunicação bidirecional via MQTT
- Execução automática de rotinas configuráveis
- Integração com backend para gerenciamento remoto

## Arquitetura

Sensores/Atuadores ⇄ ESP32 (embedded-farm) ⇄ MQTT ⇄ Backend

## Integração

Este firmware é utilizado em conjunto com:

- Backend: https://github.com/FagnerTimoteo/granja-back
- Frontend: https://github.com/joelrodriguesvieira/front-end-farm-management

## Objetivo

Permitir automação confiável, monitoramento em tempo real e controle
remoto de equipamentos em ambientes de granja.

---

# 📦 Requisitos

Antes de importar o projeto, instale:

✅ ESP-IDF (mesma versão do projeto — recomendado 5.3.x)  
✅ VSCode  
✅ Extensão C/C++ (Microsoft)
✅ Git

---

# ⬇️ 1. Instalar o ESP-IDF (Windows)

Baixe o instalador v5.3.X oficial:

👉 https://dl.espressif.com/dl/esp-idf/


Durante a instalação:

✔ Express Install  
✔ Install tools  
✔ Install Python environment  
✔ Add to PATH  

Após instalar, abra:

👉 **ESP-IDF Command Prompt**

Teste:

idf.py --version


Se aparecer a versão → OK.

---

# ⬇️ 2. Baixar o projeto

```bash
git clone https://github.com/FagnerTimoteo/embedded-farm.git
cd embedded-farm

🔧 3. Ativar ambiente ESP-IDF

Sempre rode antes de compilar:

C:\Espressif\frameworks\esp-idf-v5.3.1\export.bat


Ou abra o projeto dentro do:

👉 ESP-IDF Command Prompt

⚙️ 4. Configurar projeto

Primeira vez após baixar:

idf.py fullclean
idf.py reconfigure


Se necessário:

idf.py menuconfig

Configure:

Wi-Fi SSID

Senha

Broker MQTT

Porta MQTT

🛠️ 5. Build
idf.py build

🚀 6. Flash no ESP32

Conecte o ESP32 via USB:

idf.py -p COMx flash monitor


Exemplo:

idf.py -p COM3 flash monitor


Sair do monitor:

Ctrl + ]

🧠 7. Usar no VSCode (opcional)

Instale extensões:

ESP-IDF Extension

C/C++ (Microsoft)

Abra a pasta do projeto no VSCode.

Depois:

Ctrl + Shift + P
ESP-IDF: Open ESP-IDF Terminal


Build:

idf.py build

🧹 Corrigir erros de IntelliSense no VSCode

Se aparecer:

cannot open source file "freertos/FreeRTOS.h"


Isso é erro visual do VSCode, não do compilador.

Gere o banco de compilação:

idf.py reconfigure


Confirme que existe:

build/compile_commands.json


Crie:

.vscode/settings.json


Conteúdo:

{
  "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json"
}


Reinicie o VSCode.

📡 Funcionamento

Após boot:

✔ conecta ao Wi-Fi
✔ conecta ao broker MQTT
✔ publica dados dos sensores
✔ recebe comandos MQTT

🧯 Solução de problemas
❌ idf.py não reconhecido

Execute:

export.bat


ou abra o ESP-IDF Command Prompt.

❌ Conflito de versões do IDF

Apague:

build/


Depois:

idf.py fullclean
idf.py build

❌ IntelliSense vermelho mas build funciona

Ignore — é apenas VSCode.
Siga a seção de IntelliSense acima.

📁 Estrutura do projeto
embedded-farm/
 ├── main/
 ├── components/
 ├── CMakeLists.txt
 ├── sdkconfig

👨‍💻 Autor

Fagner Timoteo

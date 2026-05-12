# C Access Pattern Analyzer

Статический анализатор паттернов доступа к памяти в C-коде на основе LLVM IR.

Принимает C-файл, компилирует его в LLVM bitcode (через `clang-16`) и извлекает характеристики каждого обращения к памяти внутри циклов (load/store, stride, affine, зависимости и т.д.), затем сохраняет результат в `out.json`.

## 1. Сборка

```bash
cmake -S . -B build
cmake --build build -j4
```

Бинарник будет создан по пути:

```bash
./build/analyzer
```

## 2. Запуск

Основной запуск:

```bash
./build/analyzer conf.json
```

По умолчанию анализатор:
- читает вход из `conf.json` (поле `input`)
- если это `.c` файл: автоматически делает `clang-16 -O0 -Xclang -disable-O0-optnone -g -fno-inline -fno-discard-value-names -emit-llvm -c ...`
- применяет `mem2reg` (промоция alloca → SSA) внутренне
- запускает статический анализ по IR
- пишет результат в `out.json`

## 3. Конфигурация

Пример `conf.json`:

```json
{
  "input": "test.c",
  "output": "out.json",
  "analysis": {
    "max_loop_depth": 4,
    "analyze_dependencies": true,
    "analyze_scev": true
  },
  "output_format": "json",
  "debug": {
    "verbose": true,
    "dump_loops": false,
    "dump_scev": false,
    "dump_memory": false
  },
  "features": {
    "enable_fingerprint": true,
    "enable_classification": true
  }
}
```

## 4. Как работает анализ

Pipeline:

1. `main.cpp` читает конфиг.
2. Если вход `*.c`, файл компилируется в промежуточный `analyzer_input.bc`.
3. `PatternExtractor` парсит IR и обходит функции.
4. Для каждой функции:
   - промоция alloca → SSA через `PromoteMemToReg`
   - строится `LoopInfo`, `ScalarEvolution`, `DependenceAnalysis` контекст
   - `LoopCollector` собирает циклы до `max_loop_depth`
   - `MemoryAccessAnalyzer` собирает обращения к памяти в теле каждого цикла (только текущий уровень, без вложенных подциклов)
   - `ScevAnalyzer` извлекает stride/affine/multidim и связанные оценки через LLVM ScalarEvolution + ручной анализ GEP-индексов
   - `DependenceChecker` оценивает зависимости через LLVM DependenceAnalysis + AliasAnalysis
   - `PatternClassifier` присваивает `pattern_type`
   - `FingerprintBuilder` формирует стабильный идентификатор
5. `PatternSerializer` сохраняет массив паттернов в `out.json`.

## 5. Типы паттернов (pattern_type)

Анализатор классифицирует каждый доступ к памяти в одну из следующих категорий. Приоритет классификации — сверху вниз: первое подходящее правило срабатывает.

| Тип паттерна | Описание | Условие классификации | Пример C-кода | Эффективность |
|---|---|---|---|---|
| **`gather_scatter`** | Индекс зависит от чтения из памяти (косвенная индексация) | `affine=false && indexed_by_memory=true` | `a[i] = b[idx[i]]` (доступ к `b`) | Наихудшая — непредсказуемый доступ |
| **`indirect`** | Неаффинный доступ без признака indexed_by_memory | `affine=false && indexed_by_memory=false` | Сложная нелинейная адресация | Плохая |
| **`multidim_affine`** | Аффинный доступ с несколькими индуктивными переменными | `affine=true && multidim=true` | `a[((i*n1+j)*n2+k)*n3+l]` | Хорошая при unit stride по внутренней IV |
| **`unit_stride`** | Последовательный доступ с шагом 1 (или -1) в элементах | `affine=true && abs(stride)==1` | `a[i] = b[i]` | Идеальная — линейный prefetch, векторизация |
| **`constant_stride`** | Регулярный доступ с постоянным шагом > 1 | `affine=true && abs(stride)>1` | `a[2*i] = b[2*i]` | Средняя — разреженное использование cache-line |
| **`broadcast`** | Доступ к одному и тому же адресу на каждой итерации (loop-invariant) | `affine=true && stride==0` | `a[i] = b[j]` (во внутреннем цикле по `i`, `j` — внешняя IV) | Отличная — один элемент в регистре/кэше |
| **`irregular`** | Доступ не попал в более узкие категории | (зарезервирован) | — | Плохая |
| **`unknown`** | Тип не удалось определить | Все остальные случаи | Доступ вне цикла, stride=null | Неопределённая |

## 6. Описание характеристик (поля out.json)

Каждый элемент массива в `out.json` — одна запись паттерна (отдельный `load` или `store`).

### 6.1. Контекстные характеристики

| Поле | Тип | Описание |
|---|---|---|
| `sequence_index` | int | Порядковый номер паттерна в итоговом JSON после фильтрации и дедупликации |
| `function` | string | Имя функции, в которой найден доступ |
| `depth` | int | Глубина цикла: `0` — вне цикла, `1` — внешний цикл, `2+` — вложенные |
| `access_kind` | string | `"load"` (чтение) или `"store"` (запись) |
| `base_symbol` | string | Имя базового объекта (массив, указатель, SSA-значение) |
| `base_kind` | string | Тип базы: `"pointer_arg"`, `"global_array"`, `"global_pointer"`, `"stack_pointer"`, `"other"` |
| `source_file` | string | Имя исходного файла (из debug-информации) |
| `source_line` | int | Номер строки в исходном файле |
| `source_column` | int | Номер столбца в исходном файле |
| `load_count` | int | 1 для load, 0 для store |
| `store_count` | int | 1 для store, 0 для load |

### 6.2. Характеристики формы доступа

| Поле | Тип | Описание | Как вычисляется |
|---|---|---|---|
| `affine` | bool | Является ли адрес аффинной функцией от индуктивных переменных цикла (`base + a*i + b*j + c`) | SCEV AddRecExpr.isAffine() + ручной анализ GEP-индексов через getLinearCoeff |
| `stride` | int/null | Шаг доступа в элементах по текущей IV цикла. `1` = последовательный, `2+` = с пропуском, `0` = broadcast, `null` = неизвестен | SCEV step / sizeof(element), или ручной коэффициент при IV |
| `has_indexed_addressing` | bool | Есть ли GEP-инструкция в адресе | Наличие GetElementPtrInst/GEPOperator |
| `indexed_by_memory` | bool | Зависит ли индекс GEP от значения, загруженного из памяти (LoadInst в цепочке индекса) | Рекурсивный обход зависимостей GEP-индексов |
| `conditional` | bool | Находится ли доступ под условным ветвлением (if) внутри цикла | Проверка predecessors на conditional branch |

### 6.3. Количественные метрики

| Поле | Тип | Описание | Как вычисляется |
|---|---|---|---|
| `contiguous_block` | int/null | Оценка длины непрерывного блока элементов | `stride=0` → 1 (broadcast); `stride=1` → trip_count (или null); `stride>1` → `⌊cache_line / (elem_size × stride)⌋` |
| `fill_factor` | float | Плотность использования данных в cache-line, от 0 до 1 | `min(1, 1/abs(stride))`. Для conditional доступов домножается на 0.5 |
| `alignment` | int/null | Выравнивание доступа в байтах (из IR-атрибутов load/store) | `LoadInst.getAlign()` / `StoreInst.getAlign()` |
| `working_set_bytes` | int | Статическая оценка рабочего объёма данных в байтах | `abs(stride) × trip_count × sizeof(element)`. Broadcast → sizeof(element). 0 если trip count неизвестен |

### 6.4. Анализ зависимостей

| Поле | Тип | Описание | Как вычисляется |
|---|---|---|---|
| `dependence` | string | Результат анализа зависимостей | LLVM DependenceInfo + BasicAliasAnalysis. `"no-dep"` — нет зависимостей, `"loop-carried-dep"` — зависимость между итерациями, `"unknown"` — не удалось определить |

### 6.5. Классификация и отпечаток

| Поле | Тип | Описание |
|---|---|---|
| `pattern_signature` | string | Канонический fingerprint паттерна; нормализованный отпечаток вида `k=load\|p=unit_stride\|s=1\|a=1\|c=0\|m=0\|im=0\|ia=1` |
| `pattern_fingerprint` | string | Технический alias значения `pattern_signature` для интеграции с Go-сервисами и JOIN с динамическими метриками |
| `pattern_type` | string | Итоговая классификация (см. раздел 5) |

### 6.6. Как интерпретировать значения

**Хороший (эффективный) доступ:**
- `pattern_type = unit_stride` или `multidim_affine`
- `stride = 1`
- `affine = true`
- `indexed_by_memory = false`
- `conditional = false`
- `dependence = no-dep`
- `fill_factor` близок к `1.0`
- `alignment ≥ 16`

**Проблемный доступ:**
- `pattern_type = gather_scatter` или `indirect` — непредсказуемый, не векторизуется
- `stride > 1` — разреженное использование cache-line (fill_factor < 1)
- `conditional = true` — требует masked-операций при векторизации
- `dependence = loop-carried-dep` — блокирует автовекторизацию

## 7. Примеры паттернов

### unit_stride: `a[i] = b[i]`
```json
{
  "pattern_type": "unit_stride",
  "stride": 1,
  "affine": true,
  "fill_factor": 1.0,
  "conditional": false,
  "indexed_by_memory": false
}
```

### constant_stride: `a[2*i] = b[2*i]`
```json
{
  "pattern_type": "constant_stride",
  "stride": 2,
  "affine": true,
  "fill_factor": 0.5,
  "contiguous_block": 8
}
```

### gather_scatter: `a[i] = b[idx[i]]`
Доступ к `b` через массив индексов:
```json
{
  "pattern_type": "gather_scatter",
  "stride": null,
  "affine": false,
  "indexed_by_memory": true,
  "fill_factor": 0.0
}
```

### broadcast: `a[i] = b[j]` (внутренний цикл по i)
```json
{
  "pattern_type": "broadcast",
  "stride": 0,
  "affine": true,
  "fill_factor": 1.0,
  "contiguous_block": 1
}
```

### multidim_affine: `a[((i*n1+j)*n2+k)*n3+l]`
```json
{
  "pattern_type": "multidim_affine",
  "stride": 1,
  "affine": true,
  "fill_factor": 1.0,
  "depth": 4
}
```

### conditional: `if ((i&1)==0) { a[i] = b[i]; }`
```json
{
  "pattern_type": "unit_stride",
  "stride": 1,
  "affine": true,
  "conditional": true,
  "fill_factor": 0.5
}
```

## 8. Ограничения

- **working_set_bytes = 0** для циклов с символическими границами (параметр `n` вместо констант). Оценка возможна только при известном trip count на этапе компиляции.
- **contiguous_block = null** для unit_stride с неизвестным trip count — нельзя статически определить длину непрерывного блока.
- **dependence = unknown** при aliasing между указателями-аргументами (компилятор не может доказать отсутствие перекрытия без `restrict`).
- Анализатор работает с `-O0` IR после `mem2reg` промоции. Оптимизированный IR (`-O2`) может давать отличающиеся результаты из-за loop rotation, unrolling и т.д.

## 9. Docker

### Сборка образа

```bash
docker build -t analyzer .
```

Multi-stage: на этапе build устанавливаются `llvm-16-dev` + `cmake` и компилируется бинарник. В финальный образ попадают только `analyzer`, `clang-16` и минимальные runtime-библиотеки.

### Запуск

```bash
# stdout-режим (JSON → stdout, метаданные → stderr)
docker run --rm -v $(pwd):/work analyzer conf.json --stdout

# файловый режим
docker run --rm -v $(pwd):/work analyzer conf.json -o /work/result.json

# переопределение входного файла
docker run --rm -v /path/to/files:/work analyzer conf.json -i /work/source.c --stdout
```

### CLI-флаги

| Флаг | Описание |
|---|---|
| `--stdout` | JSON-результат в stdout (вместо файла) |
| `-o PATH` / `--output PATH` | Переопределить путь выходного файла |
| `-i PATH` / `--input PATH` | Переопределить путь входного файла |
| `-q` / `--quiet` | Отключить verbose-лог |

Информационные сообщения (`Analysis finished`, `Patterns found: N`) всегда идут в stderr и не мешают парсингу JSON из stdout.

## 10. Интеграция с Go-воркером

### Рекомендованная архитектура

```
Kafka → Go Worker → exec.Command("analyzer") → JSON → ClickHouse
                ↕                                    ↕
              MinIO (файлы .c)              API (результаты)
```

**Вариант A — бинарник внутри контейнера воркера** (рекомендуемый):

Go worker и analyzer в одном Docker-образе. Go вызывает бинарник как subprocess через `exec.Command`. Самый простой вариант — минимальная задержка, нет сетевого overhead.

**Вариант B — отдельный контейнер** (микросервис):

Analyzer запускается отдельно с HTTP/gRPC обёрткой. Go worker отправляет `.c`-файл, получает JSON. Подходит если нужно масштабировать анализатор отдельно от воркера.

### Вариант A: бинарник в образе Go-воркера

Dockerfile воркера:

```dockerfile
# Берём готовый бинарник из образа analyzer
FROM analyzer:latest AS analyzer-bin

FROM golang:1.22-bookworm AS builder
WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o worker ./cmd/worker

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-16 libzstd1 zlib1g libtinfo6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=analyzer-bin /usr/local/bin/analyzer /usr/local/bin/analyzer
COPY --from=builder /app/worker /usr/local/bin/worker

ENTRYPOINT ["worker"]
```

### Пример Go-кода вызова анализатора

```go
package analyzer

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

type AnalyzerConfig struct {
	Input  string `json:"input"`
	Output string `json:"output"`
	Analysis struct {
		MaxLoopDepth        int  `json:"max_loop_depth"`
		AnalyzeDependencies bool `json:"analyze_dependencies"`
		AnalyzeScev         bool `json:"analyze_scev"`
	} `json:"analysis"`
	OutputFormat string `json:"output_format"`
	Debug        struct {
		Verbose bool `json:"verbose"`
	} `json:"debug"`
	Features struct {
		EnableFingerprint      bool `json:"enable_fingerprint"`
		EnableClassification   bool `json:"enable_classification"`
	} `json:"features"`
}

type AccessPattern struct {
	Function             string   `json:"function"`
	Depth                int      `json:"depth"`
	AccessKind           string   `json:"access_kind"`
	BaseSymbol           string   `json:"base_symbol"`
	BaseKind             string   `json:"base_kind"`
	IndexedByMemory      bool     `json:"indexed_by_memory"`
	HasIndexedAddressing bool     `json:"has_indexed_addressing"`
	LoadCount            int      `json:"load_count"`
	StoreCount           int      `json:"store_count"`
	Affine               bool     `json:"affine"`
	Conditional          bool     `json:"conditional"`
	FillFactor           float64  `json:"fill_factor"`
	WorkingSetBytes      int64    `json:"working_set_bytes"`
	Dependence           string   `json:"dependence"`
	PatternType          string   `json:"pattern_type"`
	PatternSignature     string   `json:"pattern_signature"`
	SourceFile           *string  `json:"source_file"`
	SourceLine           *int     `json:"source_line"`
	SourceColumn         *int     `json:"source_column"`
	Stride               *int64   `json:"stride"`
	ContiguousBlock      *int64   `json:"contiguous_block"`
	Alignment            *int     `json:"alignment"`
}

// RunAnalysis скачивает .c файл, запускает анализатор и возвращает результат.
func RunAnalysis(ctx context.Context, sourceCode []byte, filename string) ([]AccessPattern, error) {
	workDir, err := os.MkdirTemp("", "analyzer-*")
	if err != nil {
		return nil, fmt.Errorf("create workdir: %w", err)
	}
	defer os.RemoveAll(workDir)

	// Записать исходник
	srcPath := filepath.Join(workDir, filename)
	if err := os.WriteFile(srcPath, sourceCode, 0600); err != nil {
		return nil, fmt.Errorf("write source: %w", err)
	}

	// Сгенерировать conf.json
	cfg := AnalyzerConfig{
		Input:        filename,
		Output:       "out.json",
		OutputFormat: "json",
	}
	cfg.Analysis.MaxLoopDepth = 4
	cfg.Analysis.AnalyzeDependencies = true
	cfg.Analysis.AnalyzeScev = true
	cfg.Features.EnableFingerprint = true
	cfg.Features.EnableClassification = true

	confData, err := json.Marshal(cfg)
	if err != nil {
		return nil, fmt.Errorf("marshal config: %w", err)
	}
	confPath := filepath.Join(workDir, "conf.json")
	if err := os.WriteFile(confPath, confData, 0600); err != nil {
		return nil, fmt.Errorf("write config: %w", err)
	}

	// Запустить анализатор
	cmd := exec.CommandContext(ctx, "analyzer", "conf.json", "--stdout", "-q")
	cmd.Dir = workDir
	output, err := cmd.Output()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			return nil, fmt.Errorf("analyzer failed: %s", string(exitErr.Stderr))
		}
		return nil, fmt.Errorf("run analyzer: %w", err)
	}

	// Распарсить результат
	var patterns []AccessPattern
	if err := json.Unmarshal(output, &patterns); err != nil {
		return nil, fmt.Errorf("parse output: %w", err)
	}

	return patterns, nil
}
```

### Поток данных в воркере

```
1. Kafka consumer получает сообщение {file_id, minio_path}
2. Go worker скачивает .c файл из MinIO → []byte
3. RunAnalysis(ctx, sourceCode, "input.c") → []AccessPattern
4. Вариант A: INSERT INTO clickhouse (patterns...)
   Вариант B: Отправить JSON обратно через Kafka/HTTP API
5. ACK сообщения в Kafka
```

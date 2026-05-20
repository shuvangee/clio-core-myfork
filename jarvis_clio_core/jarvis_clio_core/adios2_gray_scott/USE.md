# Gray Scott model execution

## Gray-scott with adios2 as I/O library
Please follow these steps for the gray-scott with adios2 as I/O library.
### 1. Setup Environment

Create the environment variables needed by Gray Scott
```bash
spack load openmpi
export PATH="${COEUS_Adapter/build/bin}:$PATH"
```````````



### 2. Create a Pipeline

The Jarvis pipeline will store all configuration data needed by Gray Scott.

```bash
jarvis pipeline create gray-scott-test
```

### 3. Save Environment

Store the current environment in the pipeline.
```bash
jarvis pipeline env build
```

### 4. Add pkgs to the Pipeline

Create a Jarvis pipeline with Gray Scott
```bash
jarvis pipeline append adios2_gray_scott

```

### 5. Run Experiment

Run the experiment
```bash
jarvis pipeline run
```

### 6. Clean Data

Clean data produced by Gray Scott
```bash
jarvis pipeline clean
```

## Gray Scott With Clio as I/O engine and adios2 as I/O library
Please follow this steps for the gray-scott with clio as I/O engine and adios2 as I/O libray.

### 1. Setup Environment
Create the environment variables needed by Clio + Gray Scott
```bash
spack load adios2
spack load clio@master
export PATH="${COEUS_Adapter/build/bin}:$PATH"
```


### 2. Create a Pipeline

The Jarvis pipeline will store all configuration data needed by Clio and Gray Scott.

```bash
jarvis pipeline create gs-clio
```

### 3. Save Environment

We must make Jarvis aware of all environment variables needed to execute applications in the pipeline.

```bash
jarvis pipeline env build
```

### 4. Add pkgs to the Pipeline

Create a Jarvis pipeline with Clio (theMPI-IO interceptor), and Gray-Scott

Option 1: without derived variables
```bash
jarvis pipeline append hermes_run --sleep=10 --provider=sockets
jarvis pipeline append adios2_gray_scott engine=clio 
```
Option2: with derived variables
For derived variable with adios2 in clio:
```bash
jarvis pipeline append hermes_run --sleep=10 --provider=sockets
jarvis pipeline append adios2_gray_scott engine=hermes_derived
```

### 5. Run the Experiment

Run the experiment
```bash
jarvis pipeline run
```

### 6. Clean Data

To clean data produced by Clio + Gray-Scott:
```bash
jarvis pipeline clean
```


## Gray-Scott configration file
Please refer to [README.md](README.md) for more inforamtion.

## Gray-Scott installation
Please refer to [INSTALL.md](INSTALL.md) for more information.
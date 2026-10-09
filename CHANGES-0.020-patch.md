# ps5emu 0.020 + crash-triage patch (src/emu.cpp only)

Source-only; NOT compiled on Windows here (the printf formatter was compile-tested on Linux against a real SysV va_list).

- Fault handler: for an execute-at-NULL it now decodes the `call` that jumped there (call [rip+disp], call [reg+disp], call reg),
  logs the slot address (guest vaddr), its value, whether the faulting thread is the main guest thread, and the call-site bytes.
  Send me the "faulting call:" lines from the next log - that names the null pointer.
- Real guest printf family: vsnprintf (full, incl. floats), snprintf/sprintf/printf (up to 6 int args; no xmm/stack args).
- sceKernelStat: /app0/... is mapped to the title folder and a real stat is filled in (mode/size); anything else returns ENOENT
  (was: success + zeroed buffer). First 40 calls are logged with the path.
- Out-params now written: sceUserServiceGetLoginUserIdList, sceUserServiceGetUserName, sceAppContentAppParamGetInt (always 0, id logged),
  scePadGetControllerInformation (zeroed = not connected).
- Explicit no-op HLE (return 0) for scePthread{Setaffinity,Setprio,Rename,AttrSet*}, setenv, sceUserServiceInitialize,
  sceAppContentInitialize, sceCommonDialogInitialize, scePadInit - removes them from the unresolved list.
- Known gap: the unnamed hot imports (r8mvOaWdi28#A#B x228, 35NoyMOtYpE, cJ2Y4E-t258, -pnj3-7a6QA) are still stubs; they are the
  prime suspects for the null pointer.

/***
 * Copyright 2004-present Facebook. All Rights Reserved.
 */

let root =
  Filename.(Sys.executable_name |> dirname |> dirname |> dirname |> dirname);

let project = Filename.concat(root, "sample-project");

let outputDir =
  Filename.(List.fold_left(concat, project, ["src", "__generated_flow__"]));

let outputDirLib =
  Filename.(
    List.fold_left(concat, root, ["lib", "bs", "js", "__generated_flow__"])
  );

let createModulesMap = modulesMapFile =>
  switch (modulesMapFile) {
  | None => GenFlowMain.StringMap.empty
  | Some(filePath) =>
    let s = GenFlowMain.GeneratedReFiles.readFile(filePath);
    Str.split(Str.regexp("\n"), s)
    |> List.fold_left(
         (map, nextPairStr) =>
           if (nextPairStr != "") {
             let fromTo =
               Str.split(Str.regexp("="), nextPairStr) |> Array.of_list;
             assert(Array.length(fromTo) === 2);
             let k: string = fromTo[0];
             let v: string = fromTo[1];
             GenFlowMain.StringMap.add(k, v, map);
           } else {
             map;
           },
         GenFlowMain.StringMap.empty,
       );
  };

let findCmtFiles = (): list(string) => {
  open Filename;
  let src = ["lib", "bs", "src"] |> List.fold_left(concat, project);
  ["Index.cmt", "Component1.cmt", "Component2.cmt"] |> List.map(concat(src));
};

let fileHeader =
  BuckleScriptPostProcessLib.Patterns.generatedHeaderPrefix ++ "\n\n";

let signFile = s => s;

let buildSourceFiles = () => ();
/* TODO */

let buildGeneratedFiles = () => ();
/* TODO */

let modulesMap = ref(None);
let cli = () => {
  let setModulesMap = s => modulesMap := Some(s);
  let setCmtAdd = s => {
    let splitColon = Str.split(Str.regexp(":"), s) |> Array.of_list;
    assert(Array.length(splitColon) === 2);
    let cmt: string = splitColon[0];
    let mlast: string = splitColon[1];
    let cmtPath = Filename.concat(Sys.getcwd(), cmt);
    let cmtExists = Sys.file_exists(cmtPath);
    let shouldProcess = cmtExists && Filename.check_suffix(cmtPath, ".cmt");
    if (shouldProcess) {
      print_endline("-cmt-add cmt:" ++ cmtPath ++ " mlast:" ++ mlast);
      GenFlowMain.run(
        ~outputDir,
        ~fileHeader,
        ~signFile,
        ~modulesMap=createModulesMap(modulesMap^),
        ~findCmtFiles=() => [cmtPath],
        ~buildSourceFiles,
        ~buildGeneratedFiles,
        ~doCleanup=false,
      );
    };
    exit(0);
  };
  let setCmtRm = s => {
    let splitColon = Str.split(Str.regexp(":"), s) |> Array.of_list;
    assert(Array.length(splitColon) === 1);
    let cmt: string = splitColon[0];
    let globalModuleName = Filename.chop_extension(Filename.basename(cmt));
    let re =
      Filename.concat(
        outputDir,
        GenFlowMain.Generator.outputReasonModuleName(globalModuleName)
        ++ ".re",
      );
    print_endline("-cmt-rm cmt:" ++ cmt);
    if (Sys.file_exists(re)) {
      if (Unix.fork() == 0) {
        /* the child */
        Unix.sleep(1);
        Unix.unlink(re);
      };
    };
    exit(0);
  };
  let speclist = [
    (
      "--modulesMap",
      Arg.String(setModulesMap),
      "Specify map file to override the JS module resolution for dependencies that would"
      ++ " normally be generated by genFlow but are not available for whatever reason."
      ++ " Example(--modulesMap map.txt) where each line is of the form 'ModuleFlow.bs=SomeShim'. "
      ++ "E.g. 'ReasonReactFlow.bs=ReasonReactShim'.",
    ),
    ("-cmt-add", Arg.String(setCmtAdd), "compile a .cmt file"),
    ("-cmt-rm", Arg.String(setCmtRm), "remove a .cmt file"),
  ];
  let usage = "genFlow";
  Arg.parse(speclist, print_endline, usage);

  GenFlowMain.run(
    ~outputDir,
    ~fileHeader,
    ~signFile,
    ~modulesMap=createModulesMap(modulesMap^),
    ~findCmtFiles,
    ~buildSourceFiles,
    ~buildGeneratedFiles,
    ~doCleanup=true,
  );
};

cli();
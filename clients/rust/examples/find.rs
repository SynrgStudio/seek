//! cargo run --example find -- "report .pdf" [folder]
//!
//! Finds files by name with Seek, starting it when it isn't running, and prints the best ones.

fn main() -> Result<(), seek_client::Error> {
    let mut args = std::env::args().skip(1);
    let query = args.next().unwrap_or_else(|| "report .pdf".into());
    let seek = seek_client::Seek::builder().client("find example").connect()?;
    let mut request = seek.find(query).max(20);
    if let Some(folder) = args.next() {
        request = request.path(folder);
    }
    let found = request.run()?;
    println!("{} matches{}:", found.total, if found.exact { "" } else { " (about)" });
    for e in &found.entries {
        println!("  {}{}", e.path.display(), if e.is_dir { "\\" } else { "" });
    }
    Ok(())
}
